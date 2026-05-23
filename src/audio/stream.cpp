// stream.cpp — see stream.hpp.

#include "audio/stream.hpp"
#include "audio/adpcm_encode.hpp"

#include "dsp/qmath.hpp"
#include "voices/acb_tb303_stage1.hpp"

#include "psyqo/spu.hh"

#include "common/hardware/spu.h"

namespace acid::audio::stream {

namespace {

// SPU IRQ address register (not in psyqo's wrapper). Setting bit 6 of
// SPU_CTRL enables IRQ; when the SPU reads the address stored at
// SPU_RAM_IRQ_ADDR it raises CPU IRQ 9 and latches bit 6 of SPU_STATUS.
constexpr uint32_t SPU_RAM_IRQ_ADDR_OFS = 0x1f801da4;
inline volatile uint16_t &SPU_RAM_IRQ_ADDR() {
    return *reinterpret_cast<volatile uint16_t *>(SPU_RAM_IRQ_ADDR_OFS);
}

// Active buffer index: 0 = SPU is playing A, refill B next; 1 = vice versa.
int  g_activeBuf = 0;
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
        // sin_q24 returns Q24 ∈ [-ONE, ONE]. Shift down to fit s16 with
        // ~−20 dB headroom so it's easy on the ears.
        int32_t s = sin_q24(g_sine_phase) >> 13;  // ~ ±2048
        g_pcmBuf[i] = static_cast<int16_t>(s);
    }
}

// Phase-2-only switch: while true, fill with the 1 kHz beep so we can
// verify the streaming chain by ear. Phase 3 will flip this off and route
// the ACB voice mix through fill_silence-style rendering.
bool g_test_sine_active = false;

// Phase 3: live mix of the ACB-modelled voices. Phase 3 wires only the
// TB-303 stage 1 (SAW); later phases add more voices and sum them here.
acid::voices::AcbTb303Stage1 g_tb303_stage1;

// Active fill mode. After Phase 3 we default to ACB so live render kicks
// in immediately; the OptionsScene can flip the user back to SILENT/SINE
// for debugging.
FillMode g_fillMode = FillMode::Acb;

void fill_acb_mix() {
    using acid::dsp::Q24_SHIFT;
    for (int i = 0; i < SAMPLES_PER_BUFFER; ++i) {
        // Phase 3: single voice → directly use its sample. Phase 4+
        // sums multiple voices here and clamps.
        int32_t s = g_tb303_stage1.tick();
        g_pcmBuf[i] = static_cast<int16_t>(s);
    }
}

void dma_to_spu(uint32_t spuAddr) {
    psyqo::SPU::dmaWrite(spuAddr, g_encodedBuf,
                         static_cast<uint16_t>(BYTES_PER_BUFFER), 16);
}

}  // namespace

void initialize() {
    if (g_initialized) return;

    // Pre-load both buffers with silence so the SPU has something safe to
    // play immediately and the chain A→B→A keeps running.
    switch (g_fillMode) {
        case FillMode::Sine:   fill_test_sine(); break;
        case FillMode::Acb:    fill_acb_mix();   break;
        case FillMode::Silent:
        default:               fill_silence();   break;
    }
    encode_current_buffer(SPU_BUFFER_A_ADDR, SPU_BUFFER_B_ADDR);
    dma_to_spu(SPU_BUFFER_A_ADDR);
    encode_current_buffer(SPU_BUFFER_B_ADDR, SPU_BUFFER_A_ADDR);
    dma_to_spu(SPU_BUFFER_B_ADDR);

    // Configure SPU IRQ to fire at the start of buffer B. When the SPU
    // crosses that address, bit 6 of SPU_STATUS latches and we know A is
    // done playing → refill A next. (We flip the IRQ addr on each tick.)
    SPU_CTRL = SPU_CTRL | (1u << 6);          // IRQ enable
    SPU_RAM_IRQ_ADDR() = static_cast<uint16_t>(SPU_BUFFER_B_ADDR / 8);

    // Kick playback on the stream channel. Sample rate is locked to
    // BASE_SAMPLE_RATE (44100 Hz native) — anything else would resample
    // and that's the host renderer's job, not the SPU's.
    psyqo::SPU::ChannelPlaybackConfig cfg{};
    cfg.sampleRate.value = 0x1000;            // 1.0 = 44100 Hz native
    cfg.volumeLeft  = 0x3fff;
    cfg.volumeRight = 0x3fff;
    cfg.adsr        = 0x1fffc0ff;             // sustain-forever envelope
    psyqo::SPU::playADPCM(STREAM_CHANNEL, static_cast<uint16_t>(SPU_BUFFER_A_ADDR),
                          cfg, true);
    // Override repeat addr to B so A's LOOP_END jumps to B (not back to A).
    SPU_VOICES[STREAM_CHANNEL].sampleRepeatAddr =
        static_cast<uint16_t>(SPU_BUFFER_B_ADDR / 8);

    g_activeBuf = 0;
    g_initialized = true;
}

void tick() {
    if (!g_initialized) return;

    // Check if SPU crossed the IRQ marker since last tick.
    if ((SPU_STATUS & (1u << 6)) == 0) return;

    // Acknowledge: clear IRQ enable then re-enable. This is the
    // documented way to drop the latched status bit without writing it
    // directly (which is read-only on PSX).
    SPU_CTRL = SPU_CTRL & ~(1u << 6);
    SPU_CTRL = SPU_CTRL | (1u << 6);

    // Refill the buffer the SPU just finished. After IRQ at B's start,
    // A is the one freshly-consumed → refill A and set IRQ to A's start
    // so the next firing tells us B is consumed.
    if (g_activeBuf == 0) {
        // We were playing A, IRQ fired at B start → refill A.
        switch (g_fillMode) {
        case FillMode::Sine:   fill_test_sine(); break;
        case FillMode::Acb:    fill_acb_mix();   break;
        case FillMode::Silent:
        default:               fill_silence();   break;
    }  // Phase 2: silence only.
        encode_current_buffer(SPU_BUFFER_A_ADDR, SPU_BUFFER_B_ADDR);
        // Update repeat addr so the next loop_end in B jumps back to A.
        SPU_VOICES[STREAM_CHANNEL].sampleRepeatAddr =
            static_cast<uint16_t>(SPU_BUFFER_A_ADDR / 8);
        dma_to_spu(SPU_BUFFER_A_ADDR);
        SPU_RAM_IRQ_ADDR() = static_cast<uint16_t>(SPU_BUFFER_A_ADDR / 8);
        g_activeBuf = 1;
    } else {
        switch (g_fillMode) {
        case FillMode::Sine:   fill_test_sine(); break;
        case FillMode::Acb:    fill_acb_mix();   break;
        case FillMode::Silent:
        default:               fill_silence();   break;
    }
        encode_current_buffer(SPU_BUFFER_B_ADDR, SPU_BUFFER_A_ADDR);
        SPU_VOICES[STREAM_CHANNEL].sampleRepeatAddr =
            static_cast<uint16_t>(SPU_BUFFER_B_ADDR / 8);
        dma_to_spu(SPU_BUFFER_B_ADDR);
        SPU_RAM_IRQ_ADDR() = static_cast<uint16_t>(SPU_BUFFER_B_ADDR / 8);
        g_activeBuf = 0;
    }
}

// ---- Phase 3 public API ---------------------------------------------------

void set_fill_mode(FillMode m) { g_fillMode = m; }

namespace {

inline acid::dsp::i32 byte_to_q24(int x8) {
    return (static_cast<acid::dsp::i32>(x8) << 24) / 255;
}

}  // namespace

void trigger_tb303_stage1(int noteOffset, bool slide, bool accent) {
    using acid::dsp::i32;
    // A2 = 110 Hz at noteOffset 0. Decompose offset into whole octaves +
    // fractional semitones; use pow2 LUT for the fractional part and a
    // (32-bit, MIPS-native) shift for whole octaves.
    constexpr int BASE_HZ = 110;
    int shifted = noteOffset + 12;     // map [-12, +24] → [0, 36]
    int whole_oct = shifted / 12 - 1;  // -1..+2 from the +12 shift
    int frac_semi = shifted % 12;      // 0..11
    i32 fracParam = (static_cast<i32>(frac_semi) << acid::dsp::Q24_SHIFT) / 12;
    i32 ratio = acid::dsp::pow2_unit_q24(fracParam);  // Q24 [ONE, 2*ONE]
    // BASE_HZ * ratio fits comfortably in 32-bit before the shift down.
    // Inline 32×32→64 mult + shift is the same pattern as mul_q24.
    int hz = static_cast<int>(
        (static_cast<int64_t>(BASE_HZ) * ratio) >> acid::dsp::Q24_SHIFT);
    if (whole_oct > 0)      hz <<=  whole_oct;
    else if (whole_oct < 0) hz >>= -whole_oct;
    g_tb303_stage1.noteOn(hz, slide, accent);
}

void set_tb303_stage1_knobs(int cutoff8, int reso8, int envMod8,
                            int decay8, int accent8) {
    g_tb303_stage1.setCutoff(byte_to_q24(cutoff8));
    g_tb303_stage1.setResonance(byte_to_q24(reso8));
    g_tb303_stage1.setEnvMod(byte_to_q24(envMod8));
    g_tb303_stage1.setDecay(byte_to_q24(decay8));
    g_tb303_stage1.setAccentAmount(byte_to_q24(accent8));
}

}  // namespace acid::audio::stream
