// ps1-acid-rom — PS1 sequencer (TB-303 × 2 stages + TR-808 + TR-909).
//
// Four synths grouped behind the four face buttons. Pressing a face button
// jumps the whole screen to that synth so we can use the full 320×240 for
// one machine's UI:
//
//   ×  (Cross)    → TB-303 STAGE 1   (SAW, SQR)
//   ○  (Circle)   → TB-303 STAGE 2   (SW2, SQ2)
//   □  (Square)   → TR-808           (BD, SD, TOM, HH, CY, CP, CB)
//   △  (Triangle) → TR-909           (BD9, SD9)
//
// Within the active synth (drum machines):
//   L1 / R1                 previous / next voice
//   L2                      step toggle at the cursor (active <-> off)
//   R2                      stick "fine" modifier (hold)
//   D-pad ←→                step cursor
//   D-pad ↑↓                KNOB 1 ± (PIT for drums)
//   Start                   play / stop  (when SHIFT is off)
//   Start                   chain length cycle  (when SHIFT is on)
//   Select                  toggle SHIFT mode (latching modifier)
//
// SHIFT (lit in the header when active) re-routes:
//   SHIFT + L1 / R1         KNOB 2 − / +
//   SHIFT + D-pad ←→        pattern slot prev / next
//   SHIFT + D-pad ↑↓        KNOB 1 ± on 303 (drum stays plain)
//   SHIFT + L2              accent toggle (303)
//   SHIFT + R2              slide  toggle (303)
//   SHIFT + △ (Triangle)    randomize current pattern (303)
//   SHIFT + Start           chain length cycle
//
// On TB-303 synths the D-pad ↑↓ becomes per-step note entry, plus live
// analog-stick filter control (DualShock):
//   D-pad ↑↓                cursor step note ± 1 semitone
//   Left  stick X / Y       live CUT / RES
//   Right stick X / Y       live ENV / DCY
//   R2 (hold)               while held, sticks move ~1/4 the speed (fine)
//
// We use psyqo::AdvancedPad so we can read the analog sticks; digital pads
// stay at center (0x80) and the stick contributions are no-ops in deadzone.
//
// We deliberately avoid binding to L3 / R3 — those buttons only exist on
// DualShock (analog-stick clicks). On the original PSX digital pad they
// simply don't physically exist, and SimplePad reports them as always-up.
//
// Knob change is always live — no tweak-mode flag, no separate knob page.
// 13 voices live underneath, distributed across the 4 synths; each voice
// keeps its own 16-step pattern per slot, so swapping voice or synth never
// loses your work in the others.

#include "psyqo/advancedpad.hh"
#include "psyqo/application.hh"
#include "psyqo/font.hh"
#include "psyqo/gpu.hh"
#include "psyqo/primitives/rectangles.hh"
#include "psyqo/scene.hh"
#include "psyqo/spu.hh"

#include "common/hardware/spu.h"

#include "generated/voice_samples.h"

namespace {

constexpr uint16_t HALF_RATE = 0x0800;
constexpr uint32_t HOLD_ADSR = 0x1fff80ff;

constexpr uint16_t PITCH_TABLE[25] = {
    0x0400, 0x043D, 0x047D, 0x04C2, 0x050A, 0x0557, 0x05A8, 0x05FE,
    0x065A, 0x06BA, 0x0721, 0x078D,
    0x0800,
    0x087A, 0x08FB, 0x0983, 0x0A14, 0x0AAD, 0x0B50, 0x0BFC,
    0x0CB3, 0x0D74, 0x0E41, 0x0F1A, 0x1000,
};
constexpr int PITCH_TABLE_ZERO = 12;

// Wider 3-octave table for TB-303 per-step note entry: indices 0..36 cover
// -12..+24 semitones. Each octave is exactly 2x the previous one's rate.
constexpr uint16_t PITCH_TABLE_ACID[37] = {
    0x0400, 0x043D, 0x047D, 0x04C2, 0x050A, 0x0557, 0x05A8, 0x05FE,
    0x065A, 0x06BA, 0x0721, 0x078D,
    0x0800,
    0x087A, 0x08FB, 0x0983, 0x0A14, 0x0AAD, 0x0B50, 0x0BFC,
    0x0CB3, 0x0D74, 0x0E41, 0x0F1A, 0x1000,
    0x10F4, 0x11F6, 0x1306, 0x1428, 0x155A, 0x16A0, 0x17F8,
    0x1966, 0x1AE8, 0x1C82, 0x1E34, 0x2000,
};
constexpr int PITCH_TABLE_ACID_ZERO = 12;
constexpr int PITCH_TABLE_ACID_MAX  = 36;

constexpr int NUM_VOICES   = 13;
constexpr int NUM_PATTERNS = 8;
constexpr int NUM_STEPS    = 16;
constexpr int FRAMES_PER_STEP = 8;

constexpr int NUM_ACID_VOICES = 4;  // 303 STG1 saw+sqr (idx 7,8) + STG2 saw+sqr (idx 11,12)

inline int acidSlotForVoice(int v) {
    switch (v) {
        case 7:  return 0;
        case 8:  return 1;
        case 11: return 2;
        case 12: return 3;
    }
    return -1;
}
inline bool isAcidVoice(int v) { return acidSlotForVoice(v) >= 0; }

// Per-step metadata for 303 voices. The "active" bit still lives in the
// shared m_voicePatterns bitfield; this struct only adds 303-specific
// expression that drum voices don't need.
struct AcidStep {
    int8_t  note  = 0;   // semitones; clamped to [-12, +24]
    uint8_t flags = 0;   // bit0 = accent, bit1 = slide
};
constexpr uint8_t ACID_ACCENT = 1u << 0;
constexpr uint8_t ACID_SLIDE  = 1u << 1;

struct VoiceDef {
    const char *name;
    const uint8_t *data;
    unsigned bytes;
    uint16_t volume;
    uint32_t spuAddr;
};

VoiceDef g_voices[NUM_VOICES] = {
    {"BD",  acid::voice_samples::bd_adpcm,             acid::voice_samples::bd_adpcm_bytes,             0x3000, 0},
    {"SD",  acid::voice_samples::sd_adpcm,             acid::voice_samples::sd_adpcm_bytes,             0x2400, 0},
    {"TOM", acid::voice_samples::tom_adpcm,            acid::voice_samples::tom_adpcm_bytes,            0x2800, 0},
    {"HH",  acid::voice_samples::hh_adpcm,             acid::voice_samples::hh_adpcm_bytes,             0x1800, 0},
    {"CY",  acid::voice_samples::cy_adpcm,             acid::voice_samples::cy_adpcm_bytes,             0x1c00, 0},
    {"CP",  acid::voice_samples::cp_adpcm,             acid::voice_samples::cp_adpcm_bytes,             0x2000, 0},
    {"CB",  acid::voice_samples::cb_adpcm,             acid::voice_samples::cb_adpcm_bytes,             0x1c00, 0},
    {"SAW", acid::voice_samples::tb303_saw_adpcm,      acid::voice_samples::tb303_saw_adpcm_bytes,      0x2800, 0},
    {"SQR", acid::voice_samples::tb303_square_adpcm,   acid::voice_samples::tb303_square_adpcm_bytes,   0x2400, 0},
    {"BD9", acid::voice_samples::r909bd_adpcm,         acid::voice_samples::r909bd_adpcm_bytes,         0x3000, 0},
    {"SD9", acid::voice_samples::r909sd_adpcm,         acid::voice_samples::r909sd_adpcm_bytes,         0x2400, 0},
    {"SW2", acid::voice_samples::tb303s2_saw_adpcm,    acid::voice_samples::tb303s2_saw_adpcm_bytes,    0x2800, 0},
    {"SQ2", acid::voice_samples::tb303s2_square_adpcm, acid::voice_samples::tb303s2_square_adpcm_bytes, 0x2400, 0},
};

constexpr uint8_t CH_PER_VOICE(int v) { return static_cast<uint8_t>(v); }

// Synth grouping for the face-button selector.
struct SynthDef {
    const char *shortName;   // "303", "303+", "808", "909"
    const char *longName;    // "TB-303 STAGE 1" etc.
    int firstVoice;          // index into g_voices
    int voiceCount;
    psyqo::Color color;
};

constexpr int NUM_SYNTHS = 4;
constexpr SynthDef SYNTHS[NUM_SYNTHS] = {
    {"303",  "TB-303 STAGE 1",  7, 2, {{.r = 220, .g = 110, .b =  40}}},
    {"303+", "TB-303 STAGE 2", 11, 2, {{.r = 200, .g = 100, .b = 220}}},
    {"808",  "TR-808 ANALOG",   0, 7, {{.r = 220, .g = 180, .b =  80}}},
    {"909",  "TR-909 HYBRID",   9, 2, {{.r = 220, .g =  70, .b =  60}}},
};

inline bool isAcidSynth(int s) { return s == 0 || s == 1; }

struct RowKnobs {
    uint8_t level   = 0xC0;
    int8_t  pitch   = 0;
    uint8_t tone    = 0x80;
    uint8_t decay   = 0x80;
    uint8_t cutoff  = 0x66;
    uint8_t reso    = 0xB3;
    uint8_t envMod  = 0x99;
};

constexpr int NUM_KNOBS = 4;

struct KnobSlot {
    const char *label;
    int valueMax;
};
constexpr KnobSlot ACID_KNOBS[NUM_KNOBS] = {
    {"CUT", 255}, {"RES", 255}, {"ENV", 255}, {"DCY", 255},
};
constexpr KnobSlot DRUM_KNOBS[NUM_KNOBS] = {
    {"PIT",  24}, {"TNE", 255}, {"DCY", 255}, {"LVL", 255},
};

inline const KnobSlot *knobSetForSynth(int s) {
    return isAcidSynth(s) ? ACID_KNOBS : DRUM_KNOBS;
}

inline int knobValue(const RowKnobs &k, int synth, int slot) {
    if (isAcidSynth(synth)) {
        switch (slot) {
            case 0: return k.cutoff;
            case 1: return k.reso;
            case 2: return k.envMod;
            case 3: return k.decay;
        }
    } else {
        switch (slot) {
            case 0: return k.pitch + 12;
            case 1: return k.tone;
            case 2: return k.decay;
            case 3: return k.level;
        }
    }
    return 0;
}

inline void knobBump(RowKnobs &k, int synth, int slot, int delta) {
    auto clamp8 = [](int v) { if (v < 0) v = 0; if (v > 255) v = 255; return static_cast<uint8_t>(v); };
    if (isAcidSynth(synth)) {
        switch (slot) {
            case 0: k.cutoff = clamp8(static_cast<int>(k.cutoff) + delta * 0x10); break;
            case 1: k.reso   = clamp8(static_cast<int>(k.reso)   + delta * 0x10); break;
            case 2: k.envMod = clamp8(static_cast<int>(k.envMod) + delta * 0x10); break;
            case 3: k.decay  = clamp8(static_cast<int>(k.decay)  + delta * 0x10); break;
        }
    } else {
        if (slot == 0) {
            int v = k.pitch + delta;
            if (v < -12) v = -12;
            if (v > 12)  v = 12;
            k.pitch = static_cast<int8_t>(v);
            return;
        }
        switch (slot) {
            case 1: k.tone  = clamp8(static_cast<int>(k.tone)  + delta * 0x10); break;
            case 2: k.decay = clamp8(static_cast<int>(k.decay) + delta * 0x10); break;
            case 3: k.level = clamp8(static_cast<int>(k.level) + delta * 0x10); break;
        }
    }
}

void uploadVoice(uint32_t spuAddr, const uint8_t *data, unsigned bytes) {
    psyqo::SPU::dmaWrite(spuAddr, data, static_cast<uint16_t>(bytes), 16);
}

void triggerVoice(uint8_t channel, const VoiceDef &v, const RowKnobs &k) {
    psyqo::SPU::ChannelPlaybackConfig cfg{};
    int idx = PITCH_TABLE_ZERO + k.pitch;
    if (idx < 0) idx = 0;
    if (idx > 24) idx = 24;
    cfg.sampleRate.value = PITCH_TABLE[idx];
    uint32_t vol = (static_cast<uint32_t>(v.volume) * k.level) / 255;
    if (vol > 0x7FFF) vol = 0x7FFF;
    cfg.volumeLeft  = static_cast<uint16_t>(vol);
    cfg.volumeRight = static_cast<uint16_t>(vol);
    cfg.adsr = HOLD_ADSR;
    psyqo::SPU::playADPCM(channel, static_cast<uint16_t>(v.spuAddr), cfg, true);
}

void triggerAcidVoice(uint8_t channel, const VoiceDef &v, const RowKnobs &k,
                      const AcidStep &step) {
    psyqo::SPU::ChannelPlaybackConfig cfg{};
    int idx = PITCH_TABLE_ACID_ZERO + static_cast<int>(step.note);
    if (idx < 0) idx = 0;
    if (idx > PITCH_TABLE_ACID_MAX) idx = PITCH_TABLE_ACID_MAX;
    cfg.sampleRate.value = PITCH_TABLE_ACID[idx];

    uint32_t vol = (static_cast<uint32_t>(v.volume) * k.level) / 255;
    if (step.flags & ACID_ACCENT) {
        vol = (vol * 13) / 10;  // +30% on accent
    }
    if (vol > 0x7FFF) vol = 0x7FFF;
    cfg.volumeLeft  = static_cast<uint16_t>(vol);
    cfg.volumeRight = static_cast<uint16_t>(vol);
    cfg.adsr = HOLD_ADSR;
    psyqo::SPU::playADPCM(channel, static_cast<uint16_t>(v.spuAddr), cfg, true);
}

// ----------------------------------------------------------------------------
namespace reverb {

constexpr uint32_t WORK_AREA_START_BYTES = 0x70000;
volatile uint16_t *const REVERB_REGS = reinterpret_cast<volatile uint16_t *>(0x1F801DC0);

constexpr uint16_t HALL[32] = {
    0x007D, 0x005B, 0x6D80, 0x54B8,
    0x4954, 0x4504, 0x3E40, 0xC97C,
    0x4FA0, 0x5040, 0x55D8, 0x5570,
    0x4FA8, 0x4F60, 0x4938, 0x48F0,
    0x55D0, 0x5568, 0x4ABA, 0x4990,
    0x484C, 0x48E8, 0x42D8, 0x42E8,
    0x44A0, 0x44B0, 0x42E8, 0x4334,
    0x42DC, 0x4328, 0x4000, 0x4000,
};

bool g_enabled = false;
uint32_t g_channelMask = 0;

void setup() {
    SPU_CTRL = SPU_CTRL & ~(1u << 7);
    SPU_REVERB_ADDR = static_cast<uint16_t>(WORK_AREA_START_BYTES / 8);
    for (int i = 0; i < 32; ++i) REVERB_REGS[i] = HALL[i];
    SPU_REVERB_LEFT  = 0x3000;
    SPU_REVERB_RIGHT = 0x3000;
    SPU_REVERB_EN_LOW  = 0;
    SPU_REVERB_EN_HIGH = 0;
    SPU_CTRL = SPU_CTRL | (1u << 7);
}

void apply() {
    SPU_REVERB_EN_LOW  = g_enabled ? static_cast<uint16_t>( g_channelMask        & 0xFFFFu) : 0;
    SPU_REVERB_EN_HIGH = g_enabled ? static_cast<uint16_t>((g_channelMask >> 16) & 0xFFFFu) : 0;
}

void setVoiceSend(int voice, bool on) {
    uint32_t bit = 1u << CH_PER_VOICE(voice);
    if (on) g_channelMask |=  bit;
    else    g_channelMask &= ~bit;
    apply();
}

void setEnabled(bool on) { g_enabled = on; apply(); }
bool enabled() { return g_enabled; }
bool voiceSend(int voice) { return (g_channelMask >> CH_PER_VOICE(voice)) & 1u; }

}  // namespace reverb

class AcidRom final : public psyqo::Application {
    void prepare() override;
    void createScene() override;

  public:
    psyqo::Font<> m_font;
    psyqo::AdvancedPad m_input;
    bool m_initialized = false;
};

class SequencerScene final : public psyqo::Scene {
    void start(StartReason reason) override;
    void frame() override;

    void draw();
    void handleInput();
    void advancePlayback();
    void randomizeAcidPattern(int acidSlot, int voiceIdx);

    int selectedVoice() const {
        return SYNTHS[m_currentSynth].firstVoice + m_voiceInSynth[m_currentSynth];
    }

    uint16_t m_voicePatterns[NUM_PATTERNS][NUM_VOICES] = {};
    AcidStep m_acidExtras[NUM_PATTERNS][NUM_ACID_VOICES][NUM_STEPS] = {};

    int m_currentSynth         = 2;          // 808 by default — fastest reward on boot
    int m_voiceInSynth[NUM_SYNTHS] = {0, 0, 0, 0};

    int m_currentPattern = 0;
    int m_playingPattern = 0;
    int m_chainLength    = 1;
    int m_chainPos       = 0;
    int m_cursorStep     = 0;
    int m_playStep       = 0;
    uint32_t m_frameCounter = 0;
    bool m_running = true;

    RowKnobs m_knobs[NUM_VOICES];

    // SHIFT is a latching modifier toggled by every Select edge-press.
    // When on, all buttons that previously had Select-chord meanings
    // produce those meanings instead of their default ones.
    bool m_shiftMode = false;

    uint16_t m_prevButtons[2] = {0, 0};

    // LCG state for randomize; re-seeded with frame counter on each press
    // so consecutive randomizes diverge.
    uint32_t m_randState = 0xDEADBEEFu;
    uint32_t lcg() {
        m_randState = m_randState * 1664525u + 1013904223u;
        return m_randState;
    }
};

AcidRom acidRom;
SequencerScene sequencerScene;

}  // namespace

void AcidRom::prepare() {
    psyqo::GPU::Configuration config;
    config.set(psyqo::GPU::Resolution::W320)
        .set(psyqo::GPU::VideoMode::AUTO)
        .set(psyqo::GPU::ColorMode::C15BITS)
        .set(psyqo::GPU::Interlace::PROGRESSIVE);
    gpu().initialize(config);

    psyqo::SPU::initialize();

    uint32_t cursor = 0x1100;
    for (auto &v : g_voices) {
        v.spuAddr = cursor;
        uploadVoice(v.spuAddr, v.data, v.bytes);
        cursor += v.bytes;
    }

    reverb::setup();

    // PollingMode::Fast = poll all ports each vsync. Acid-line wobbling is
    // latency-sensitive so we eat the CPU cost. AdvancedPad initialization
    // is recommended from prepare() (unlike SimplePad which needed BIOS to
    // be active before initialize).
    m_input.initialize(psyqo::AdvancedPad::PollingMode::Fast);
}

void AcidRom::createScene() {
    if (!m_initialized) {
        m_font.uploadSystemFont(gpu());
        m_initialized = true;
    }
    pushScene(&sequencerScene);
}

void SequencerScene::start(StartReason) {
    // Default seed pattern: a small live-ish groove.
    m_voicePatterns[0][0]  = 0b1000100010001000;  // BD 4-on-the-floor
    m_voicePatterns[0][1]  = 0b0000100000001000;  // SD on backbeat
    m_voicePatterns[0][3]  = 0b0101010101010101;  // HH off-eighths
    m_voicePatterns[0][7]  = 0b0010001000100010;  // SAW (303 STG1) accents
}

void SequencerScene::handleInput() {
    using B = psyqo::AdvancedPad::Button;
    using P = psyqo::AdvancedPad::Pad;
    auto &pad = acidRom.m_input;
    auto press = [&](B b) {
        bool now = pad.isButtonPressed(P::Pad1a, b);
        bool wasDown = (m_prevButtons[0] & (1 << b)) == 0;
        return now && !wasDown;
    };

    // SHIFT is a latching modifier: every Select edge-press toggles it,
    // and the LED in the header shows the state. No more "tap vs chord"
    // disambiguation, no holding required.
    if (press(B::Select)) m_shiftMode = !m_shiftMode;
    const bool shift = m_shiftMode;

    // Face buttons → synth select by default. SHIFT + Triangle on a 303
    // synth is captured as the randomize chord and does NOT jump to 909.
    if (press(B::Cross))    m_currentSynth = 0;   // ×  303 STG1
    if (press(B::Circle))   m_currentSynth = 1;   // ○  303 STG2
    if (press(B::Square))   m_currentSynth = 2;   // □  808
    if (press(B::Triangle)) {
        int v0 = selectedVoice();
        int slot0 = acidSlotForVoice(v0);
        if (shift && slot0 >= 0) {
            randomizeAcidPattern(slot0, v0);
        } else {
            m_currentSynth = 3;
        }
    }

    const SynthDef &syn = SYNTHS[m_currentSynth];

    int v = selectedVoice();

    // L1 / R1: voice prev / next (default), or KNOB 2 ± when SHIFT.
    if (press(B::L1)) {
        if (shift) knobBump(m_knobs[v], m_currentSynth, 1, -1);
        else       m_voiceInSynth[m_currentSynth] = (m_voiceInSynth[m_currentSynth] + syn.voiceCount - 1) % syn.voiceCount;
    }
    if (press(B::R1)) {
        if (shift) knobBump(m_knobs[v], m_currentSynth, 1, +1);
        else       m_voiceInSynth[m_currentSynth] = (m_voiceInSynth[m_currentSynth] + 1) % syn.voiceCount;
    }

    // Re-read v after voice change (in case L1/R1 changed it without SHIFT).
    v = selectedVoice();
    int acidSlot = acidSlotForVoice(v);
    bool isAcid = acidSlot >= 0;

    // L2: step toggle at cursor (active <-> off). With SHIFT on a 303
    // voice it becomes the per-step accent toggle.
    if (press(B::L2)) {
        if (shift && isAcid) {
            m_acidExtras[m_currentPattern][acidSlot][m_cursorStep].flags ^= ACID_ACCENT;
        } else {
            m_voicePatterns[m_currentPattern][v] ^= (uint16_t(1) << m_cursorStep);
        }
    }
    // R2: stick "fine" modifier (read separately as a hold below). With
    // SHIFT on a 303 voice the edge press toggles per-step slide.
    if (press(B::R2) && shift && isAcid) {
        m_acidExtras[m_currentPattern][acidSlot][m_cursorStep].flags ^= ACID_SLIDE;
    }

    // D-pad LR: step cursor (default), or pattern slot prev/next under SHIFT.
    if (press(B::Left)) {
        if (shift) m_currentPattern = (m_currentPattern + NUM_PATTERNS - 1) % NUM_PATTERNS;
        else       m_cursorStep = (m_cursorStep + NUM_STEPS - 1) % NUM_STEPS;
    }
    if (press(B::Right)) {
        if (shift) m_currentPattern = (m_currentPattern + 1) % NUM_PATTERNS;
        else       m_cursorStep = (m_cursorStep + 1) % NUM_STEPS;
    }

    // D-pad UD. Drum synths always bump KNOB 1. On 303 synths the default
    // action is per-step note (TB-303 style); SHIFT bumps KNOB 1 (CUT).
    auto bumpNote = [&](int delta) {
        auto &s = m_acidExtras[m_currentPattern][acidSlot][m_cursorStep];
        int n = static_cast<int>(s.note) + delta;
        if (n < -12) n = -12;
        if (n >  24) n =  24;
        s.note = static_cast<int8_t>(n);
    };
    if (press(B::Up)) {
        if (isAcid && !shift) bumpNote(+1);
        else                  knobBump(m_knobs[v], m_currentSynth, 0, +1);
    }
    if (press(B::Down)) {
        if (isAcid && !shift) bumpNote(-1);
        else                  knobBump(m_knobs[v], m_currentSynth, 0, -1);
    }

    // Start: play/stop by default; under SHIFT it cycles chain length.
    if (press(B::Start)) {
        if (shift) {
            m_chainLength = (m_chainLength % NUM_PATTERNS) + 1;
            m_chainPos = 0;
        } else {
            m_running = !m_running;
        }
    }

    // Analog sticks → live knob bumps (303 mode only). DualShock in analog
    // mode reports center=0x80, edge=0x00/0xFF. Digital pads or analog-LED-off
    // DualShocks stay parked at 0x80 so the deadzone makes this a no-op.
    // R2 hold drops the bump speed to ~1/4 for fine adjustments.
    if (isAcid) {
        bool r2Held = pad.isButtonPressed(P::Pad1a, B::R2);
        int divisor = r2Held ? 32 : 8;
        auto bumpByStick = [&](uint8_t a, uint8_t &target) {
            int d = static_cast<int>(a) - 0x80;
            if (d > -0x10 && d < 0x10) return;
            int s = d / divisor;
            if (s == 0) s = (d > 0) ? 1 : -1;
            int n = static_cast<int>(target) + s;
            if (n < 0) n = 0;
            if (n > 255) n = 255;
            target = static_cast<uint8_t>(n);
        };
        RowKnobs &k = m_knobs[v];
        bumpByStick(pad.getAdc(P::Pad1a, 2), k.cutoff);  // LeftJoyX  → CUT
        bumpByStick(pad.getAdc(P::Pad1a, 3), k.reso);    // LeftJoyY  → RES
        bumpByStick(pad.getAdc(P::Pad1a, 0), k.envMod);  // RightJoyX → ENV
        bumpByStick(pad.getAdc(P::Pad1a, 1), k.decay);   // RightJoyY → DCY
    }

    uint16_t bits = 0;
    for (int b = 0; b < 16; ++b) {
        if (!pad.isButtonPressed(P::Pad1a, static_cast<B>(b))) {
            bits |= (1 << b);
        }
    }
    m_prevButtons[0] = bits;
}

void SequencerScene::advancePlayback() {
    if (!m_running) return;
    if ((m_frameCounter % FRAMES_PER_STEP) != 0) return;

    int newStep = (m_frameCounter / FRAMES_PER_STEP) % NUM_STEPS;
    if (newStep == 0 && m_playStep != 0 && m_chainLength > 1) {
        m_chainPos = (m_chainPos + 1) % m_chainLength;
    }
    m_playStep = newStep;
    m_playingPattern = (m_chainLength > 1) ? m_chainPos : m_currentPattern;

    for (int v = 0; v < NUM_VOICES; ++v) {
        if (m_voicePatterns[m_playingPattern][v] & (uint16_t(1) << m_playStep)) {
            int slot = acidSlotForVoice(v);
            if (slot >= 0) {
                triggerAcidVoice(CH_PER_VOICE(v), g_voices[v], m_knobs[v],
                                 m_acidExtras[m_playingPattern][slot][m_playStep]);
            } else {
                triggerVoice(CH_PER_VOICE(v), g_voices[v], m_knobs[v]);
            }
        }
    }
}

void SequencerScene::randomizeAcidPattern(int acidSlot, int voiceIdx) {
    // Re-seed with frame counter so successive randomizes produce different
    // patterns even though the LCG itself is deterministic.
    m_randState ^= m_frameCounter * 0x9E3779B1u + 0xCAFEBABEu;

    uint16_t mask = 0;
    for (int s = 0; s < NUM_STEPS; ++s) {
        uint32_t r = lcg();

        // ~62% step density — sparse enough to leave gaps, dense enough
        // that 16 steps usually feel like a line.
        bool active = ((r & 0xFF) < 0xA0);
        if (active) mask |= (uint16_t(1) << s);

        AcidStep &step = m_acidExtras[m_currentPattern][acidSlot][s];

        // Note pool: pentatonic-ish range within an octave, biased low
        // so the line still sounds like a bassline rather than a melody.
        static constexpr int8_t POOL[16] = {
             0,  0,  3,  5,
             7,  7, 10, 12,
            -2, -5,  0,  2,
             5,  7,  3, -7,
        };
        step.note = POOL[(r >> 8) & 0xF];

        uint8_t flags = 0;
        if (active) {
            if (((r >> 16) & 0xFF) < 0x55) flags |= ACID_ACCENT;  // ~33%
            if (((r >> 24) & 0xFF) < 0x40) flags |= ACID_SLIDE;   // ~25%
        }
        step.flags = flags;
    }
    m_voicePatterns[m_currentPattern][voiceIdx] = mask;
}

void SequencerScene::draw() {
    psyqo::Color white{{.r = 240, .g = 240, .b = 240}};
    psyqo::Color cyan {{.r =  60, .g = 200, .b = 220}};
    psyqo::Color dim  {{.r =  90, .g = 100, .b = 115}};
    psyqo::Color amber{{.r = 240, .g = 180, .b =  60}};

    const SynthDef &syn = SYNTHS[m_currentSynth];
    psyqo::Color scol = syn.color;
    int v = selectedVoice();
    const RowKnobs &k = m_knobs[v];

    // ============ Header ============
    psyqo::Prim::Rectangle headerBg{{{.r = 24, .g = 28, .b = 36}}};
    headerBg.position = {{.x = 0, .y = 0}};
    headerBg.size     = {{.x = 320, .y = 22}};
    acidRom.gpu().sendPrimitive(headerBg);
    acidRom.m_font.print(acidRom.gpu(), "ps1-acid-rom", {{.x = 8, .y = 6}}, cyan);
    acidRom.m_font.print(acidRom.gpu(),
        m_running ? "PLAY" : "STOP", {{.x = 108, .y = 6}}, m_running ? cyan : dim);
    char patBuf[8];
    patBuf[0]='P'; patBuf[1]='A'; patBuf[2]='T'; patBuf[3]=' ';
    patBuf[4]='1'+static_cast<char>(m_currentPattern); patBuf[5]='/';
    patBuf[6]='0'+static_cast<char>(NUM_PATTERNS); patBuf[7]='\0';
    acidRom.m_font.print(acidRom.gpu(), patBuf, {{.x = 152, .y = 6}}, amber);
    char chnBuf[8];
    chnBuf[0]='C'; chnBuf[1]='H'; chnBuf[2]='N'; chnBuf[3]=' ';
    chnBuf[4]='1'+static_cast<char>(m_chainLength-1); chnBuf[5]='\0';
    acidRom.m_font.print(acidRom.gpu(), chnBuf, {{.x = 208, .y = 6}}, white);
    acidRom.m_font.print(acidRom.gpu(),
        reverb::enabled() ? "REV" : "DRY",
        {{.x = 248, .y = 6}}, reverb::enabled() ? cyan : dim);
    // SHIFT latch indicator: bright amber when modifier mode is on so the
    // user always knows whether a button does its default or shifted action.
    acidRom.m_font.print(acidRom.gpu(),
        m_shiftMode ? "SHIFT" : "shift",
        {{.x = 272, .y = 6}}, m_shiftMode ? amber : dim);

    // ============ Synth tabs (× ○ □ △ row) ============
    // Four equal-width tabs across the top, each labeled with both the face
    // glyph and the synth short name. Active tab takes the synth color.
    constexpr int TAB_Y = 26;
    constexpr int TAB_H = 22;
    constexpr int TAB_W = 76;
    constexpr int TAB_X0 = 8;
    constexpr int TAB_GAP = 4;
    const char *glyphs[4] = {"X", "O", "[]", "/\\"};
    for (int s = 0; s < NUM_SYNTHS; ++s) {
        int x = TAB_X0 + s * (TAB_W + TAB_GAP);
        psyqo::Color tabCol = (s == m_currentSynth)
            ? SYNTHS[s].color
            : psyqo::Color{{.r = static_cast<uint8_t>(SYNTHS[s].color.r >> 3),
                            .g = static_cast<uint8_t>(SYNTHS[s].color.g >> 3),
                            .b = static_cast<uint8_t>(SYNTHS[s].color.b >> 3)}};
        psyqo::Prim::Rectangle tab{tabCol};
        tab.position = {{.x = static_cast<int16_t>(x), .y = TAB_Y}};
        tab.size     = {{.x = TAB_W, .y = TAB_H}};
        acidRom.gpu().sendPrimitive(tab);
        psyqo::Color labelCol = (s == m_currentSynth) ? white : dim;
        acidRom.m_font.print(acidRom.gpu(), glyphs[s],
                            {{.x = static_cast<int16_t>(x + 4), .y = TAB_Y + 6}}, labelCol);
        acidRom.m_font.print(acidRom.gpu(), SYNTHS[s].shortName,
                            {{.x = static_cast<int16_t>(x + 20), .y = TAB_Y + 6}}, labelCol);
    }

    // ============ Voice display + voice picker ============
    constexpr int VOICE_Y = 56;
    acidRom.m_font.print(acidRom.gpu(), syn.longName,
                        {{.x = 8, .y = VOICE_Y}}, scol);
    acidRom.m_font.print(acidRom.gpu(), "VOICE", {{.x = 8, .y = VOICE_Y + 14}}, dim);
    acidRom.m_font.print(acidRom.gpu(), g_voices[v].name,
                        {{.x = 48, .y = VOICE_Y + 14}}, white);

    // Voice picker chips on the right side of the voice info area.
    constexpr int PICK_X0 = 110;
    constexpr int PICK_Y0 = VOICE_Y + 10;
    constexpr int PICK_W  = 28;
    constexpr int PICK_H  = 20;
    constexpr int PICK_GAP = 2;
    for (int i = 0; i < syn.voiceCount; ++i) {
        int x = PICK_X0 + i * (PICK_W + PICK_GAP);
        int voiceIdx = syn.firstVoice + i;
        bool isSelected = (i == m_voiceInSynth[m_currentSynth]);
        bool hasNotes   = m_voicePatterns[m_currentPattern][voiceIdx] != 0;
        psyqo::Color c = scol;
        if (!isSelected && !hasNotes) { c.r >>= 2; c.g >>= 2; c.b >>= 2; }
        else if (!isSelected)         { c.r >>= 1; c.g >>= 1; c.b >>= 1; }
        psyqo::Prim::Rectangle box{c};
        box.position = {{.x = static_cast<int16_t>(x), .y = PICK_Y0}};
        box.size     = {{.x = PICK_W, .y = PICK_H}};
        acidRom.gpu().sendPrimitive(box);
        if (isSelected) {
            psyqo::Prim::Rectangle bT{white}, bB{white}, bL{white}, bR{white};
            bT.position = {{.x = static_cast<int16_t>(x - 1), .y = PICK_Y0 - 1}};
            bT.size     = {{.x = PICK_W + 2, .y = 1}};
            bB.position = {{.x = static_cast<int16_t>(x - 1), .y = PICK_Y0 + PICK_H}};
            bB.size     = {{.x = PICK_W + 2, .y = 1}};
            bL.position = {{.x = static_cast<int16_t>(x - 1), .y = PICK_Y0}};
            bL.size     = {{.x = 1, .y = PICK_H}};
            bR.position = {{.x = static_cast<int16_t>(x + PICK_W), .y = PICK_Y0}};
            bR.size     = {{.x = 1, .y = PICK_H}};
            acidRom.gpu().sendPrimitive(bT);
            acidRom.gpu().sendPrimitive(bB);
            acidRom.gpu().sendPrimitive(bL);
            acidRom.gpu().sendPrimitive(bR);
        }
        acidRom.m_font.print(acidRom.gpu(), g_voices[voiceIdx].name,
                            {{.x = static_cast<int16_t>(x + 4), .y = PICK_Y0 + 6}},
                            isSelected ? white : dim);
    }

    // On 303 synths, show the cursor step's per-step parameters (note, accent,
    // slide) in the space to the right of the voice picker.
    {
        int curAcidSlotPanel = acidSlotForVoice(v);
        if (curAcidSlotPanel >= 0) {
            const AcidStep &as = m_acidExtras[m_currentPattern][curAcidSlotPanel][m_cursorStep];
            int detailX = PICK_X0 + syn.voiceCount * (PICK_W + PICK_GAP) + 12;
            int detailY = PICK_Y0;
            // Line 1: "S nn  N+/-NN"
            char top[10];
            int sn = m_cursorStep + 1;
            top[0] = 'S';
            top[1] = '0' + static_cast<char>((sn / 10) % 10);
            top[2] = '0' + static_cast<char>(sn % 10);
            top[3] = ' ';
            top[4] = 'N';
            int n = as.note;
            top[5] = (n < 0) ? '-' : '+';
            if (n < 0) n = -n;
            if (n > 99) n = 99;
            top[6] = '0' + static_cast<char>((n / 10) % 10);
            top[7] = '0' + static_cast<char>(n % 10);
            top[8] = '\0';
            acidRom.m_font.print(acidRom.gpu(), top,
                {{.x = static_cast<int16_t>(detailX),
                  .y = static_cast<int16_t>(detailY)}}, white);

            // Line 2: accent / slide tags. Lit when flag is on.
            acidRom.m_font.print(acidRom.gpu(), "ACC",
                {{.x = static_cast<int16_t>(detailX),
                  .y = static_cast<int16_t>(detailY + 12)}},
                (as.flags & ACID_ACCENT) ? amber : dim);
            acidRom.m_font.print(acidRom.gpu(), "SLD",
                {{.x = static_cast<int16_t>(detailX + 32),
                  .y = static_cast<int16_t>(detailY + 12)}},
                (as.flags & ACID_SLIDE) ? amber : dim);
        }
    }

    // ============ Step row (16 LEDs) ============
    constexpr int STEP_Y    = 100;
    constexpr int STEP_X0   = 16;
    constexpr int STEP_W    = 16;
    constexpr int STEP_H    = 26;
    constexpr int STEP_GAP  = 3;

    uint16_t pattern = m_voicePatterns[m_currentPattern][v];
    int curAcidSlot = acidSlotForVoice(v);
    bool curIsAcid  = curAcidSlot >= 0;
    for (int s = 0; s < NUM_STEPS; ++s) {
        int x = STEP_X0 + s * (STEP_W + STEP_GAP);
        bool active   = (pattern & (uint16_t(1) << s)) != 0;
        bool playing  = m_running && (s == m_playStep);
        bool isCursor = (s == m_cursorStep);

        bool stepAccent = false;
        bool stepSlide  = false;
        if (curIsAcid) {
            const AcidStep &as = m_acidExtras[m_currentPattern][curAcidSlot][s];
            stepAccent = (as.flags & ACID_ACCENT) != 0;
            stepSlide  = (as.flags & ACID_SLIDE)  != 0;
        }

        psyqo::Color c = scol;
        if (!active) { c.r >>= 3; c.g >>= 3; c.b >>= 3; }
        else if (stepAccent) {
            c.r = c.r > 200 ? 255 : c.r + 55;
            c.g = c.g > 200 ? 255 : c.g + 55;
            c.b = c.b > 200 ? 255 : c.b + 55;
        }
        if (playing && active) {
            c.r = c.r > 200 ? 255 : c.r + 55;
            c.g = c.g > 200 ? 255 : c.g + 55;
            c.b = c.b > 200 ? 255 : c.b + 55;
        }
        psyqo::Prim::Rectangle led{c};
        led.position = {{.x = static_cast<int16_t>(x), .y = STEP_Y}};
        led.size     = {{.x = STEP_W, .y = STEP_H}};
        acidRom.gpu().sendPrimitive(led);

        // Accent tag: tiny bright bar across the top edge of an active step.
        if (curIsAcid && active && stepAccent) {
            psyqo::Prim::Rectangle accTag{white};
            accTag.position = {{.x = static_cast<int16_t>(x + 2), .y = STEP_Y + 2}};
            accTag.size     = {{.x = STEP_W - 4, .y = 2}};
            acidRom.gpu().sendPrimitive(accTag);
        }
        // Slide tag: underline at the bottom — tying current to next step.
        if (curIsAcid && active && stepSlide) {
            psyqo::Prim::Rectangle slTag{amber};
            slTag.position = {{.x = static_cast<int16_t>(x + 2), .y = STEP_Y + STEP_H - 4}};
            slTag.size     = {{.x = STEP_W - 4, .y = 2}};
            acidRom.gpu().sendPrimitive(slTag);
        }

        if ((s & 3) == 0) {
            psyqo::Prim::Rectangle qb{cyan};
            qb.position = {{.x = static_cast<int16_t>(x + 6), .y = STEP_Y - 5}};
            qb.size     = {{.x = 4, .y = 2}};
            acidRom.gpu().sendPrimitive(qb);
        }
        if (isCursor) {
            psyqo::Prim::Rectangle top{white}, bot{white}, lft{white}, rgt{white};
            top.position = {{.x = static_cast<int16_t>(x - 1), .y = STEP_Y - 1}};
            top.size     = {{.x = STEP_W + 2, .y = 1}};
            bot.position = {{.x = static_cast<int16_t>(x - 1), .y = STEP_Y + STEP_H}};
            bot.size     = {{.x = STEP_W + 2, .y = 1}};
            lft.position = {{.x = static_cast<int16_t>(x - 1), .y = STEP_Y}};
            lft.size     = {{.x = 1, .y = STEP_H}};
            rgt.position = {{.x = static_cast<int16_t>(x + STEP_W), .y = STEP_Y}};
            rgt.size     = {{.x = 1, .y = STEP_H}};
            acidRom.gpu().sendPrimitive(top);
            acidRom.gpu().sendPrimitive(bot);
            acidRom.gpu().sendPrimitive(lft);
            acidRom.gpu().sendPrimitive(rgt);
        }
    }
    // Ruler under step row.
    for (int s = 0; s < NUM_STEPS; ++s) {
        char d[3];
        if (s < 9) { d[0] = '1' + static_cast<char>(s); d[1] = '\0'; }
        else { d[0] = '1'; d[1] = '0' + static_cast<char>((s + 1) % 10); d[2] = '\0'; }
        acidRom.m_font.print(acidRom.gpu(), d,
                            {{.x = static_cast<int16_t>(STEP_X0 + 4 + s * (STEP_W + STEP_GAP)),
                              .y = STEP_Y + STEP_H + 2}},
                            ((s & 3) == 0) ? amber : dim);
    }

    // ============ Inline knob bank ============
    // 4 fader bars all the time. Knob slots 0 + 1 are live (D-pad UD and
    // L3/R3 respectively); slots 2 + 3 are state-only readouts until
    // M2-live makes them audible.
    constexpr int KFX0  = 16;
    constexpr int KFY   = 152;
    constexpr int KFW   = 60;
    constexpr int KFH   = 48;
    constexpr int KFSTP = 76;
    const KnobSlot *slots = knobSetForSynth(m_currentSynth);
    const bool acidMode = isAcidSynth(m_currentSynth);
    for (int slot = 0; slot < NUM_KNOBS; ++slot) {
        int x = KFX0 + slot * KFSTP;
        // On 303 the four sticks drive all four knobs live; on drums only
        // slot 0 (D-pad UD) and slot 1 (Select+L1/R1) move from input.
        bool live = acidMode ? true : (slot == 0 || slot == 1);
        psyqo::Color box{{.r = 14, .g = 18, .b = 25}};
        psyqo::Prim::Rectangle outer{box};
        outer.position = {{.x = static_cast<int16_t>(x), .y = KFY}};
        outer.size     = {{.x = KFW, .y = KFH}};
        acidRom.gpu().sendPrimitive(outer);

        int value = knobValue(k, m_currentSynth, slot);
        int fillW = (KFW - 2) * value / slots[slot].valueMax;
        if (fillW < 0) fillW = 0;
        if (fillW > KFW - 2) fillW = KFW - 2;
        psyqo::Prim::Rectangle fill{scol};
        fill.position = {{.x = static_cast<int16_t>(x + 1), .y = static_cast<int16_t>(KFY + KFH - 14)}};
        fill.size     = {{.x = static_cast<int16_t>(fillW), .y = 8}};
        acidRom.gpu().sendPrimitive(fill);

        // Cyan border on live slots.
        if (live) {
            psyqo::Prim::Rectangle bT{cyan}, bB{cyan}, bL{cyan}, bR{cyan};
            bT.position = {{.x = static_cast<int16_t>(x - 1), .y = KFY - 1}};
            bT.size     = {{.x = KFW + 2, .y = 1}};
            bB.position = {{.x = static_cast<int16_t>(x - 1), .y = KFY + KFH}};
            bB.size     = {{.x = KFW + 2, .y = 1}};
            bL.position = {{.x = static_cast<int16_t>(x - 1), .y = KFY}};
            bL.size     = {{.x = 1, .y = KFH}};
            bR.position = {{.x = static_cast<int16_t>(x + KFW), .y = KFY}};
            bR.size     = {{.x = 1, .y = KFH}};
            acidRom.gpu().sendPrimitive(bT);
            acidRom.gpu().sendPrimitive(bB);
            acidRom.gpu().sendPrimitive(bL);
            acidRom.gpu().sendPrimitive(bR);
        }

        acidRom.m_font.print(acidRom.gpu(), slots[slot].label,
                            {{.x = static_cast<int16_t>(x + 4), .y = KFY + 4}},
                            live ? cyan : dim);

        // Numeric.
        char b[5];
        int displayVal = value;
        if (slot == 0 && !isAcidSynth(m_currentSynth)) {
            displayVal = k.pitch;
        }
        if (displayVal < 0) { b[0] = '-'; displayVal = -displayVal; }
        else                 { b[0] = ' '; }
        b[1] = '0' + ((displayVal / 100) % 10);
        b[2] = '0' + ((displayVal / 10)  % 10);
        b[3] = '0' + ( displayVal        % 10);
        b[4] = '\0';
        acidRom.m_font.print(acidRom.gpu(), b,
                            {{.x = static_cast<int16_t>(x + 28), .y = KFY + 4}}, white);

        // Hint glyph under the slot label. On 303 we label each knob with
        // its stick axis (LJX/LJY/RJX/RJY); on drums we mark the two slots
        // that are wired to D-pad / shoulder chords.
        if (acidMode) {
            const char *stickHint = "LJX";
            switch (slot) {
                case 0: stickHint = "LJX"; break;
                case 1: stickHint = "LJY"; break;
                case 2: stickHint = "RJX"; break;
                case 3: stickHint = "RJY"; break;
            }
            acidRom.m_font.print(acidRom.gpu(), stickHint,
                                {{.x = static_cast<int16_t>(x + KFW - 24), .y = KFY + 4}}, dim);
        } else if (slot == 0) {
            acidRom.m_font.print(acidRom.gpu(), "UD",
                                {{.x = static_cast<int16_t>(x + KFW - 16), .y = KFY + 4}}, dim);
        } else if (slot == 1) {
            acidRom.m_font.print(acidRom.gpu(), "BS+QR",
                                {{.x = static_cast<int16_t>(x + KFW - 32), .y = KFY + 4}}, dim);
        }
    }

    // ============ Footer help ============
    // BS now toggles SHIFT (not held). Help shows the action for the
    // current SHIFT state so the user can read what each button will do.
    if (isAcidSynth(m_currentSynth)) {
        if (m_shiftMode) {
            acidRom.m_font.print(acidRom.gpu(),
                "[SHF] A:acc F:sld S:rnd LR:pat UD:CUT",
                {{.x = 4, .y = 208}}, amber);
            acidRom.m_font.print(acidRom.gpu(),
                "Q/R:RES Enter:chain  BS:shift off",
                {{.x = 4, .y = 220}}, dim);
        } else {
            acidRom.m_font.print(acidRom.gpu(),
                "XDZS:syn Q/R:voi UD:note A:tgl LR:cur",
                {{.x = 4, .y = 208}}, dim);
            acidRom.m_font.print(acidRom.gpu(),
                "L/RStk:knob R2:fine Enter:play BS:SHF",
                {{.x = 4, .y = 220}}, dim);
        }
    } else {
        if (m_shiftMode) {
            acidRom.m_font.print(acidRom.gpu(),
                "[SHF] LR:pat Q/R:k2 Enter:chain",
                {{.x = 4, .y = 208}}, amber);
            acidRom.m_font.print(acidRom.gpu(),
                "BS:shift off",
                {{.x = 4, .y = 220}}, dim);
        } else {
            acidRom.m_font.print(acidRom.gpu(),
                "XDZS:syn Q/R:voi A:step tgl LR:cur",
                {{.x = 4, .y = 208}}, dim);
            acidRom.m_font.print(acidRom.gpu(),
                "UD:k1 Enter:play BS:SHF",
                {{.x = 4, .y = 220}}, dim);
        }
    }
}

void SequencerScene::frame() {
    psyqo::Color bg{{.r = 8, .g = 4, .b = 16}};
    acidRom.gpu().clear(bg);

    handleInput();
    advancePlayback();
    draw();

    // M9 PSVJ sync stripe — keep the contract.
    auto stripeCell = [&](int idx, uint8_t r, uint8_t g, uint8_t b) {
        psyqo::Prim::Rectangle cell{{{.r = r, .g = g, .b = b}}};
        cell.position = {{.x = static_cast<int16_t>(304 + idx * 4), .y = 0}};
        cell.size     = {{.x = 4, .y = 4}};
        acidRom.gpu().sendPrimitive(cell);
    };
    int v = selectedVoice();
    stripeCell(0, static_cast<uint8_t>(m_playStep),
                  static_cast<uint8_t>(m_chainLength),
                  0x00);
    stripeCell(1, static_cast<uint8_t>(m_playingPattern),
                  static_cast<uint8_t>(m_currentPattern),
                  reverb::enabled() ? 1 : 0);
    stripeCell(2, static_cast<uint8_t>(v),
                  m_running ? 0xff : 0x00,
                  static_cast<uint8_t>(m_currentSynth));
    stripeCell(3, 0x00, 0x00, 0x00);

    m_frameCounter++;
}

int main() { return acidRom.run(); }
