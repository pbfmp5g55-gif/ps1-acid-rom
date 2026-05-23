// stream.cpp — see stream.hpp.

#include "audio/stream.hpp"
#include "audio/adpcm_encode.hpp"

#include "dsp/qmath.hpp"
#include "voices/acb_tb303_stage1.hpp"
#include "voices/acb_tb303_stage2.hpp"
#include "voices/acb_808_bd.hpp"
#include "voices/acb_808_sd.hpp"
#include "voices/acb_808_tom.hpp"
#include "voices/acb_808_cp.hpp"
#include "voices/acb_909_bd.hpp"
#include "voices/acb_909_sd.hpp"
#include "voices/acb_808_cb.hpp"
#include "voices/acb_808_hh.hpp"
#include "voices/acb_808_cy.hpp"

#include "psyqo/spu.hh"

#include "common/hardware/spu.h"

namespace acid::audio::stream {

namespace {

bool g_initialized = false;

// Local PCM scratch — one buffer's worth of 16-bit samples. We encode in
// place to ADPCM in `g_encodedBuf` and DMA that into SPU RAM.
int16_t g_pcmBuf[SAMPLES_PER_BUFFER];
alignas(4) uint8_t g_encodedBuf[BYTES_PER_BUFFER];

// Encode `g_pcmBuf` to `g_encodedBuf`, setting the LOOP flags so the last
// block jumps to the *other* buffer.
void encode_current_buffer(uint32_t selfAddr, uint32_t nextAddr) {
    (void)selfAddr;
    (void)nextAddr;
    for (int b = 0; b < BLOCKS_PER_BUFFER; ++b) {
        uint8_t flags = 0;
        if (b == BLOCKS_PER_BUFFER - 1) {
            // Last block: tell SPU "end of block, jump to repeat addr".
            // The repeat addr is set on the SPU voice register, not here.
            flags = adpcm::FLAG_LOOP_END | adpcm::FLAG_LOOP_ON;
        }
        adpcm::encode_block(&g_pcmBuf[b * SAMPLES_PER_BLOCK],
                            flags,
                            &g_encodedBuf[b * BYTES_PER_BLOCK]);
    }
}

void fill_silence() {
    for (int i = 0; i < SAMPLES_PER_BUFFER; ++i) g_pcmBuf[i] = 0;
}

// Phase 2 verification: a low-amplitude 1 kHz sine. If the streaming chain
// is wired correctly, this hums continuously without clicks at every
// buffer boundary. The phase accumulator persists across buffers so the
// sine stays phase-continuous.
//
// phase_inc = 2^32 * 1000 Hz / 44100 Hz = 97391607 (approx).
constexpr uint32_t SINE_PHASE_INC_1KHZ = 97391607u;
uint32_t g_sine_phase = 0;

void fill_test_sine() {
    using acid::dsp::sin_q24;
    for (int i = 0; i < SAMPLES_PER_BUFFER; ++i) {
        g_sine_phase += SINE_PHASE_INC_1KHZ;
        // Full i16 range. SPU master defaults to 0x3fff (-6 dB) and we'll
        // override to 0x7fff in initialize() so the chain is +0 dBFS.
        int32_t s = sin_q24(g_sine_phase) >> 9;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        g_pcmBuf[i] = static_cast<int16_t>(s);
    }
}

// Phase-2-only switch: while true, fill with the 1 kHz beep so we can
// verify the streaming chain by ear. Phase 3 will flip this off and route
// the ACB voice mix through fill_silence-style rendering.
bool g_test_sine_active = false;

// Phase 3-7 covered: 10 voices live (303×4 + 808 BD/SD/TOM/CP + 909 BD/SD).
acid::voices::AcbTb303Stage1 g_tb303_stg1_saw;
acid::voices::AcbTb303Stage1 g_tb303_stg1_sqr;
acid::voices::AcbTb303Stage2 g_tb303_stg2_saw;
acid::voices::AcbTb303Stage2 g_tb303_stg2_sqr;
acid::voices::Acb808Bd       g_808_bd;
acid::voices::Acb808Sd       g_808_sd;
acid::voices::Acb808Tom      g_808_tom;
acid::voices::Acb808Cp       g_808_cp;
acid::voices::Acb909Bd       g_909_bd;
acid::voices::Acb909Sd       g_909_sd;
acid::voices::Acb808Cb       g_808_cb;
acid::voices::Acb808Hh       g_808_hh;
acid::voices::Acb808Cy       g_808_cy;
bool g_voices_initialized = false;

// Which voice indices (in the main NUM_VOICES table) are live in the ACB
// engine. Toggled per phase; bit set = engine handles, bit clear = SPU.
// Voice table reminder:
//   0=BD 1=SD 2=TOM 3=HH 4=CY 5=CP 6=CB 7=SAW 8=SQR 9=BD9 10=SD9
//   11=SW2 12=SQ2
// CPU budget: 13 voices was too heavy on the R3000A. Trimmed to the
// 4 "acid line" voices (303 STG1 SAW/SQR + 808 BD/SD) so each tick is
// ~4× cheaper. The other voices stay sample-based (SPU channel) until
// we either lower the internal sample rate or add per-voice gating.
constexpr uint32_t LIVE_VOICE_MASK =
    (1u << 0) | (1u << 1) | (1u << 7) | (1u << 8);  // BD, SD, SAW, SQR

void ensure_voices_initialized() {
    if (g_voices_initialized) return;
    g_tb303_stg1_saw.setWave(acid::voices::AcbWave::Saw);
    g_tb303_stg1_sqr.setWave(acid::voices::AcbWave::Square);
    g_tb303_stg2_saw.setWave(acid::voices::AcbWave::Saw);
    g_tb303_stg2_sqr.setWave(acid::voices::AcbWave::Square);
    g_voices_initialized = true;
}

// Boot in Silent so Logo/Title/Menu aren't crushed by per-frame 13-voice
// tick. SequencerScene::start() switches us to Acb when the user actually
// enters the sequencer.
FillMode g_fillMode = FillMode::Silent;

void fill_acb_mix() {
    ensure_voices_initialized();
    // Trimmed-down mix to fit inside the R3000A's budget: only the 4
    // voices in LIVE_VOICE_MASK get ticked. The rest of NUM_VOICES still
    // exist as instances (to keep the dispatch switch happy) but aren't
    // sampled — they stay silent.
    for (int i = 0; i < SAMPLES_PER_BUFFER; ++i) {
        int32_t s = static_cast<int32_t>(g_808_bd.tick()) +
                    static_cast<int32_t>(g_808_sd.tick()) +
                    static_cast<int32_t>(g_tb303_stg1_saw.tick()) +
                    static_cast<int32_t>(g_tb303_stg1_sqr.tick());
        s >>= 2;  // 4-voice mix, -12 dB headroom
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        g_pcmBuf[i] = static_cast<int16_t>(s);
    }
}

void dma_to_spu(uint32_t spuAddr) {
    psyqo::SPU::dmaWrite(spuAddr, g_encodedBuf,
                         static_cast<uint16_t>(BYTES_PER_BUFFER), 16);
}

}  // namespace

// Simplified design: single buffer A, looped forever via LOOP_END+LOOP_ON
// with repeat addr = A itself. Every frame we re-render the buffer and
// DMA-overwrite. The SPU race-glitches at the overwrite point (one ADPCM
// block, ~635 µs) but stays seamless across rewrites because the voice
// phase accumulators carry the audio forward correctly.
//
// We swapped to this from the IRQ-based double-buffer chain because the
// previous design stayed silent under pcsx-redux — its SPU emulator may
// not flag SPU_STATUS bit 6 reliably. Single-buffer/no-IRQ doesn't depend
// on SPU IRQ behaviour at all.

void render_into_pcm_buf() {
    switch (g_fillMode) {
        case FillMode::Sine:   fill_test_sine(); break;
        case FillMode::Acb:    fill_acb_mix();   break;
        case FillMode::Silent:
        default:               fill_silence();   break;
    }
}

void initialize() {
    if (g_initialized) return;

    render_into_pcm_buf();
    encode_current_buffer(SPU_BUFFER_A_ADDR, SPU_BUFFER_A_ADDR);
    dma_to_spu(SPU_BUFFER_A_ADDR);

    // BUG WORKAROUND: psyqo::SPU::playADPCM takes spuRamAddress as a
    // uint16_t **byte address**, then divides by 8 internally. That means
    // it can only address the lower 0xFFFF bytes (64 KB) of SPU RAM.
    // Our stream buffer at 0x30000 truncates to 0x0000 — SPU starts reading
    // from RAM 0 which is silent garbage. We bypass psyqo and write the
    // SPU voice registers ourselves using the 8-byte-unit address (0x6000),
    // which fits fine in a uint16_t.
    constexpr int CH = STREAM_CHANNEL;
    constexpr uint16_t addr8 = static_cast<uint16_t>(SPU_BUFFER_A_ADDR / 8);

    // Key off first (we may be replacing the silenced dummy on this channel).
    if (CH >= 16) SPU_KEY_OFF_HIGH = 1u << (CH - 16);
    else          SPU_KEY_OFF_LOW  = 1u << CH;

    SPU_VOICES[CH].volumeLeft      = 0x3fff;
    SPU_VOICES[CH].volumeRight     = 0x3fff;
    SPU_VOICES[CH].sampleRate      = 0x1000;
    SPU_VOICES[CH].sampleStartAddr = addr8;
    SPU_VOICES[CH].ad              = 0x80ff;
    SPU_VOICES[CH].sr              = 0x1fff;
    SPU_VOICES[CH].sampleRepeatAddr = addr8;

    if (CH >= 16) SPU_KEY_ON_HIGH = 1u << (CH - 16);
    else          SPU_KEY_ON_LOW  = 1u << CH;

    g_initialized = true;
}

void tick() {
    if (!g_initialized) return;
    render_into_pcm_buf();
    encode_current_buffer(SPU_BUFFER_A_ADDR, SPU_BUFFER_A_ADDR);
    dma_to_spu(SPU_BUFFER_A_ADDR);
}

// ---- Phase 3 public API ---------------------------------------------------

void set_fill_mode(FillMode m) { g_fillMode = m; }

namespace {

inline acid::dsp::i32 byte_to_q24(int x8) {
    return (static_cast<acid::dsp::i32>(x8) << 24) / 255;
}

}  // namespace

namespace {

int note_offset_to_hz(int noteOffset) {
    using acid::dsp::i32;
    constexpr int BASE_HZ = 110;
    int shifted = noteOffset + 12;
    int whole_oct = shifted / 12 - 1;
    int frac_semi = shifted % 12;
    i32 fracParam = (static_cast<i32>(frac_semi) << acid::dsp::Q24_SHIFT) / 12;
    i32 ratio = acid::dsp::pow2_unit_q24(fracParam);
    int hz = static_cast<int>(
        (static_cast<int64_t>(BASE_HZ) * ratio) >> acid::dsp::Q24_SHIFT);
    if (whole_oct > 0)      hz <<=  whole_oct;
    else if (whole_oct < 0) hz >>= -whole_oct;
    return hz;
}

}  // namespace

bool is_voice_live(int voiceIdx) {
    if (voiceIdx < 0 || voiceIdx >= 32) return false;
    return (LIVE_VOICE_MASK & (1u << voiceIdx)) != 0;
}

void trigger_acb_voice(int voiceIdx, int noteOffset, bool slide, bool accent) {
    ensure_voices_initialized();
    // Drum voices ignore note/slide — accent boosts the velocity instead.
    constexpr acid::dsp::i32 VEL_NORMAL = (4 * (1 << acid::dsp::Q24_SHIFT)) / 5;
    constexpr acid::dsp::i32 VEL_ACCENT = 1 << acid::dsp::Q24_SHIFT;
    acid::dsp::i32 vel = accent ? VEL_ACCENT : VEL_NORMAL;

    switch (voiceIdx) {
        case 0:  g_808_bd.trigger(vel); break;
        case 1:  g_808_sd.trigger(vel); break;
        case 2:  g_808_tom.trigger(vel); break;
        case 5:  g_808_cp.trigger(vel); break;
        case 3:  g_808_hh.trigger(vel); break;
        case 4:  g_808_cy.trigger(vel); break;
        case 6:  g_808_cb.trigger(vel); break;
        case 7:  g_tb303_stg1_saw.noteOn(note_offset_to_hz(noteOffset), slide, accent); break;
        case 8:  g_tb303_stg1_sqr.noteOn(note_offset_to_hz(noteOffset), slide, accent); break;
        case 9:  g_909_bd.trigger(vel); break;
        case 10: g_909_sd.trigger(vel); break;
        case 11: g_tb303_stg2_saw.noteOn(note_offset_to_hz(noteOffset), slide, accent); break;
        case 12: g_tb303_stg2_sqr.noteOn(note_offset_to_hz(noteOffset), slide, accent); break;
    }
}

void set_acb_voice_knobs(int voiceIdx, int cutoff8, int reso8, int envMod8,
                         int decay8, int accent8) {
    ensure_voices_initialized();
    // Both stages share the same knob signature, so a generic lambda
    // dispatches by voice index without virtual calls.
    auto apply = [&](auto &v) {
        v.setCutoff(byte_to_q24(cutoff8));
        v.setResonance(byte_to_q24(reso8));
        v.setEnvMod(byte_to_q24(envMod8));
        v.setDecay(byte_to_q24(decay8));
        v.setAccentAmount(byte_to_q24(accent8));
    };
    switch (voiceIdx) {
        case 7:  apply(g_tb303_stg1_saw); break;
        case 8:  apply(g_tb303_stg1_sqr); break;
        case 11: apply(g_tb303_stg2_saw); break;
        case 12: apply(g_tb303_stg2_sqr); break;
    }
}

void set_acb_drum_knobs(int voiceIdx, int pit, int tone8, int decay8,
                        int level8) {
    ensure_voices_initialized();
    // level8 is used by 909 BD (click attack); other voices ignore it
    // until Phase 8 wires master-volume per voice.
    int pit_clamped = pit < -12 ? -12 : (pit > 12 ? 12 : pit);
    acid::dsp::i32 tuningQ24 = static_cast<acid::dsp::i32>(
        ((pit_clamped + 12) << acid::dsp::Q24_SHIFT) / 24);
    acid::dsp::i32 toneQ24  = byte_to_q24(tone8);
    acid::dsp::i32 decayQ24 = byte_to_q24(decay8);
    switch (voiceIdx) {
        case 0:
            g_808_bd.setTuning(tuningQ24);
            g_808_bd.setTone(toneQ24);
            g_808_bd.setDecay(decayQ24);
            break;
        case 1:
            g_808_sd.setTuning(tuningQ24);
            g_808_sd.setSnappy(toneQ24);
            g_808_sd.setDecay(decayQ24);
            break;
        case 2:
            g_808_tom.setTuning(tuningQ24);
            g_808_tom.setTone(toneQ24);
            g_808_tom.setDecay(decayQ24);
            break;
        case 5:
            g_808_cp.setTuning(tuningQ24);
            g_808_cp.setDecay(decayQ24);
            break;
        case 3:
            // HH knobs: pit→tune, tone→brightness, decay→openness.
            g_808_hh.setTune(tuningQ24);
            g_808_hh.setBrightness(toneQ24);
            g_808_hh.setOpenness(decayQ24);
            break;
        case 4:
            // CY: pit→tune, tone→brightness, decay→decay.
            g_808_cy.setTune(tuningQ24);
            g_808_cy.setBrightness(toneQ24);
            g_808_cy.setDecay(decayQ24);
            break;
        case 6:
            g_808_cb.setTuning(tuningQ24);
            g_808_cb.setTone(toneQ24);
            g_808_cb.setDecay(decayQ24);
            break;
        case 9:
            g_909_bd.setTuning(tuningQ24);
            g_909_bd.setTone(toneQ24);
            g_909_bd.setDecay(decayQ24);
            g_909_bd.setAttack(static_cast<acid::dsp::i32>(level8) <<
                              (acid::dsp::Q24_SHIFT - 8));  // LVL → click amt
            break;
        case 10:
            g_909_sd.setTuning(tuningQ24);
            g_909_sd.setSnappy(toneQ24);
            g_909_sd.setDecay(decayQ24);
            break;
    }
}

}  // namespace acid::audio::stream
