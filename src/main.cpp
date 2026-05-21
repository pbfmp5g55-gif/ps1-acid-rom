// ps1-acid-rom — PS1 sequencer (TB-303 × 2 stages + TR-808 + TR-909).
//
// Drum-machine workflow:
//   - 13 voices, each with its own 16-step sequencer per pattern.
//   - The display shows ONE voice's 16-step row at a time. Triangle / Circle
//     (or D-pad up/down) flips to the previous / next voice. During playback
//     all voices play simultaneously off their own steps.
//   - 8 patterns × 13 voices = 104 sequencer slots, plus a chain-length knob
//     (Select) for cycling 1..8 patterns.
//   - SimplePad pad 1:
//       D-pad ←→ : step cursor within the visible voice
//       D-pad ↑↓ : prev / next voice (alt: Triangle / Circle)
//       Cross   : step toggle on the visible voice
//       Square  : clear the visible voice's pattern
//       Start   : play / pause
//       Select  : chain length cycle 1..8
//       L1 / R1 : prev / next pattern slot to edit
//       L2      : global reverb on / off
//       L3      : reverb send on / off for the visible voice
//       R2      : enter / leave the knob page (per-voice LVL/PIT/TNE/DCY)
//
// Voice samples were rendered host-side by host_tests/gen_voice_samples (same
// C++ DSP source) and embedded as PSX ADPCM in src/generated/voice_samples.h.

#include "psyqo/application.hh"
#include "psyqo/font.hh"
#include "psyqo/gpu.hh"
#include "psyqo/primitives/rectangles.hh"
#include "psyqo/scene.hh"
#include "psyqo/simplepad.hh"
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

constexpr int NUM_VOICES   = 13;
constexpr int NUM_PATTERNS = 8;
constexpr int NUM_STEPS    = 16;
constexpr int FRAMES_PER_STEP = 8;   // ~112 BPM @ 60 Hz NTSC

struct VoiceDef {
    const char *name;        // ≤3 chars for inline label
    const uint8_t *data;
    unsigned bytes;
    uint16_t volume;
    uint32_t spuAddr;        // assigned at boot
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

// SPU channel allocation: one channel per voice = voice index. PS1 SPU has 24
// channels so all 13 voices coexist with room left.
constexpr uint8_t CH_PER_VOICE(int v) { return static_cast<uint8_t>(v); }

// Family color — used to tint LEDs / labels so 303 / 808 / 909 stay visually
// distinct even though they share one sequencer view.
psyqo::Color voiceColor(int v) {
    if (v <= 6)  return {{.r = 220, .g = 180, .b =  80}};   // 808 family — cream
    if (v <= 8)  return {{.r = 220, .g = 110, .b =  40}};   // 303 stage 1 — orange
    if (v <= 10) return {{.r = 220, .g =  70, .b =  60}};   // 909 family — red
    return {{.r = 200, .g = 100, .b = 220}};                // 303 stage 2 — magenta
}

struct RowKnobs {
    uint8_t level = 0xC0;
    int8_t  pitch = 0;
    uint8_t tone  = 0x80;
    uint8_t decay = 0x80;
};

constexpr int NUM_KNOBS = 4;
constexpr const char *KNOB_LABELS[NUM_KNOBS] = {"LVL", "PIT", "TNE", "DCY"};

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

// ----------------------------------------------------------------------------
// SPU reverb (M8) — Sony "Hall" preset.

namespace reverb {

constexpr uint32_t WORK_AREA_START_BYTES = 0x70000;
volatile uint16_t *const REVERB_REGS = reinterpret_cast<volatile uint16_t *>(0x1F801DC0);

constexpr uint16_t HALL[32] = {
    0x007D, 0x005B,
    0x6D80, 0x54B8,
    0x4954, 0x4504, 0x3E40, 0xC97C,
    0x4FA0, 0x5040,
    0x55D8, 0x5570, 0x4FA8, 0x4F60, 0x4938, 0x48F0,
    0x55D0, 0x5568, 0x4ABA, 0x4990,
    0x484C, 0x48E8, 0x42D8, 0x42E8,
    0x44A0, 0x44B0,
    0x42E8, 0x4334, 0x42DC, 0x4328,
    0x4000, 0x4000,
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
    psyqo::SimplePad m_input;
    bool m_initialized = false;
};

class SequencerScene final : public psyqo::Scene {
    void start(StartReason reason) override;
    void frame() override;

    void drawSequencerPage();
    void drawKnobPage();
    void handleInput();
    void handleSequencerInput();
    void handleKnobPageInput();
    void advancePlayback();

    // Per-pattern × per-voice 16-step bitmap. Bit N of m_voicePatterns[p][v]
    // = step N is on for voice v in pattern p.
    uint16_t m_voicePatterns[NUM_PATTERNS][NUM_VOICES] = {};

    int m_selectedVoice  = 0;
    int m_currentPattern = 0;
    int m_playingPattern = 0;
    int m_chainLength    = 1;
    int m_chainPos       = 0;
    int m_cursorStep     = 0;
    int m_playStep       = 0;
    uint32_t m_frameCounter = 0;
    bool m_running = true;

    bool m_knobPage = false;
    int  m_knobCursor = 0;
    RowKnobs m_knobs[NUM_VOICES];

    uint16_t m_prevButtons[2] = {0, 0};
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
}

void AcidRom::createScene() {
    if (!m_initialized) {
        m_font.uploadSystemFont(gpu());
        m_input.initialize();
        m_initialized = true;
    }
    pushScene(&sequencerScene);
}

void SequencerScene::start(StartReason) {
    // Seed the default pattern at slot 0 so first boot has something audible.
    // Voice indices: 0=BD, 1=SD, 3=HH, 7=SAW, 9=BD9.
    m_voicePatterns[0][0]  = 0b1000100010001000;  // BD 4-on-the-floor
    m_voicePatterns[0][1]  = 0b0000100000001000;  // SD on backbeat
    m_voicePatterns[0][3]  = 0b0101010101010101;  // HH off-eighths
    m_voicePatterns[0][7]  = 0b0010001000100010;  // SAW (303A) accents
    m_voicePatterns[0][9]  = 0b0000000000000000;  // BD9 empty (alternate kit)
}

void SequencerScene::handleInput() {
    using B = psyqo::SimplePad::Button;
    auto &pad = acidRom.m_input;
    auto press = [&](B b) {
        bool now = pad.isButtonPressed(psyqo::SimplePad::Pad1, b);
        bool wasDown = (m_prevButtons[0] & (1 << b)) == 0;
        return now && !wasDown;
    };

    if (press(B::R2)) {
        m_knobPage = !m_knobPage;
        m_knobCursor = 0;
    }

    if (m_knobPage) handleKnobPageInput();
    else            handleSequencerInput();

    if (press(B::Start)) m_running = !m_running;

    uint16_t bits = 0;
    for (int b = 0; b < 16; ++b) {
        if (!pad.isButtonPressed(psyqo::SimplePad::Pad1, static_cast<B>(b))) {
            bits |= (1 << b);
        }
    }
    m_prevButtons[0] = bits;
}

void SequencerScene::handleSequencerInput() {
    using B = psyqo::SimplePad::Button;
    auto &pad = acidRom.m_input;
    auto press = [&](B b) {
        bool now = pad.isButtonPressed(psyqo::SimplePad::Pad1, b);
        bool wasDown = (m_prevButtons[0] & (1 << b)) == 0;
        return now && !wasDown;
    };

    if (press(B::Left))  m_cursorStep = (m_cursorStep + NUM_STEPS - 1) % NUM_STEPS;
    if (press(B::Right)) m_cursorStep = (m_cursorStep + 1) % NUM_STEPS;
    // Up / Down = previous / next voice (in addition to Triangle / Circle so
    // either side of the pad can do it).
    if (press(B::Up))    m_selectedVoice = (m_selectedVoice + NUM_VOICES - 1) % NUM_VOICES;
    if (press(B::Down))  m_selectedVoice = (m_selectedVoice + 1) % NUM_VOICES;
    if (press(B::Triangle)) m_selectedVoice = (m_selectedVoice + 1) % NUM_VOICES;
    if (press(B::Circle))   m_selectedVoice = (m_selectedVoice + NUM_VOICES - 1) % NUM_VOICES;

    if (press(B::Cross))  m_voicePatterns[m_currentPattern][m_selectedVoice] ^= (uint16_t(1) << m_cursorStep);
    if (press(B::Square)) m_voicePatterns[m_currentPattern][m_selectedVoice] = 0;

    if (press(B::L1)) m_currentPattern = (m_currentPattern + NUM_PATTERNS - 1) % NUM_PATTERNS;
    if (press(B::R1)) m_currentPattern = (m_currentPattern + 1) % NUM_PATTERNS;

    if (press(B::Select)) {
        m_chainLength = (m_chainLength % NUM_PATTERNS) + 1;
        m_chainPos = 0;
    }

    if (press(B::L2)) reverb::setEnabled(!reverb::enabled());
    if (press(B::L3)) reverb::setVoiceSend(m_selectedVoice, !reverb::voiceSend(m_selectedVoice));
}

void SequencerScene::handleKnobPageInput() {
    using B = psyqo::SimplePad::Button;
    auto &pad = acidRom.m_input;
    auto press = [&](B b) {
        bool now = pad.isButtonPressed(psyqo::SimplePad::Pad1, b);
        bool wasDown = (m_prevButtons[0] & (1 << b)) == 0;
        return now && !wasDown;
    };

    if (press(B::Left))  m_knobCursor = (m_knobCursor + NUM_KNOBS - 1) % NUM_KNOBS;
    if (press(B::Right)) m_knobCursor = (m_knobCursor + 1) % NUM_KNOBS;
    if (press(B::Up))    m_selectedVoice = (m_selectedVoice + NUM_VOICES - 1) % NUM_VOICES;
    if (press(B::Down))  m_selectedVoice = (m_selectedVoice + 1) % NUM_VOICES;
    if (press(B::Triangle)) m_selectedVoice = (m_selectedVoice + 1) % NUM_VOICES;
    if (press(B::Circle))   m_selectedVoice = (m_selectedVoice + NUM_VOICES - 1) % NUM_VOICES;

    auto bump = [&](int delta) {
        RowKnobs &k = m_knobs[m_selectedVoice];
        switch (m_knobCursor) {
            case 0: { int v = static_cast<int>(k.level) + delta * 0x10; if (v < 0) v = 0; if (v > 255) v = 255; k.level = static_cast<uint8_t>(v); break; }
            case 1: { int v = k.pitch + delta; if (v < -12) v = -12; if (v > 12) v = 12; k.pitch = static_cast<int8_t>(v); break; }
            case 2: { int v = static_cast<int>(k.tone) + delta * 0x10; if (v < 0) v = 0; if (v > 255) v = 255; k.tone = static_cast<uint8_t>(v); break; }
            case 3: { int v = static_cast<int>(k.decay) + delta * 0x10; if (v < 0) v = 0; if (v > 255) v = 255; k.decay = static_cast<uint8_t>(v); break; }
        }
    };
    if (press(B::Cross))  bump(+1);
    if (press(B::Square)) bump(-1);

    if (press(B::L1)) m_currentPattern = (m_currentPattern + NUM_PATTERNS - 1) % NUM_PATTERNS;
    if (press(B::R1)) m_currentPattern = (m_currentPattern + 1) % NUM_PATTERNS;
    if (press(B::L2)) reverb::setEnabled(!reverb::enabled());
    if (press(B::L3)) reverb::setVoiceSend(m_selectedVoice, !reverb::voiceSend(m_selectedVoice));
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
            triggerVoice(CH_PER_VOICE(v), g_voices[v], m_knobs[v]);
        }
    }
}

void SequencerScene::drawSequencerPage() {
    psyqo::Color white{{.r = 240, .g = 240, .b = 240}};
    psyqo::Color cyan {{.r =  60, .g = 200, .b = 220}};
    psyqo::Color dim  {{.r =  90, .g = 100, .b = 115}};
    psyqo::Color amber{{.r = 240, .g = 180, .b =  60}};
    psyqo::Color vcol = voiceColor(m_selectedVoice);

    // Header bar.
    psyqo::Prim::Rectangle headerBg{{{.r = 24, .g = 28, .b = 36}}};
    headerBg.position = {{.x = 0, .y = 0}};
    headerBg.size     = {{.x = 320, .y = 22}};
    acidRom.gpu().sendPrimitive(headerBg);
    acidRom.m_font.print(acidRom.gpu(), "ps1-acid-rom", {{.x = 8, .y = 6}}, cyan);
    acidRom.m_font.print(acidRom.gpu(),
        m_running ? "PLAY" : "STOP", {{.x = 116, .y = 6}}, m_running ? cyan : dim);
    char patBuf[8];
    patBuf[0] = 'P'; patBuf[1] = 'A'; patBuf[2] = 'T'; patBuf[3] = ' ';
    patBuf[4] = '1' + static_cast<char>(m_currentPattern); patBuf[5] = '/';
    patBuf[6] = '0' + static_cast<char>(NUM_PATTERNS);
    patBuf[7] = '\0';
    acidRom.m_font.print(acidRom.gpu(), patBuf, {{.x = 160, .y = 6}}, amber);
    char chnBuf[8];
    chnBuf[0] = 'C'; chnBuf[1] = 'H'; chnBuf[2] = 'N'; chnBuf[3] = ' ';
    chnBuf[4] = '1' + static_cast<char>(m_chainLength - 1);
    chnBuf[5] = '\0';
    acidRom.m_font.print(acidRom.gpu(), chnBuf, {{.x = 216, .y = 6}}, white);
    acidRom.m_font.print(acidRom.gpu(),
        reverb::enabled() ? "REV" : "DRY",
        {{.x = 264, .y = 6}}, reverb::enabled() ? cyan : dim);

    // ============ Big voice display ============
    // Voice name shown large-ish in the center top, with family color square
    // and reverb-send badge.
    psyqo::Prim::Rectangle vBox{vcol};
    vBox.position = {{.x = 16, .y = 36}};
    vBox.size     = {{.x = 70, .y = 56}};
    acidRom.gpu().sendPrimitive(vBox);
    psyqo::Prim::Rectangle vBoxFrame{{{.r = 10, .g = 14, .b = 22}}};
    vBoxFrame.position = {{.x = 18, .y = 38}};
    vBoxFrame.size     = {{.x = 66, .y = 52}};
    acidRom.gpu().sendPrimitive(vBoxFrame);
    acidRom.m_font.print(acidRom.gpu(), g_voices[m_selectedVoice].name,
                        {{.x = 30, .y = 58}}, vcol);
    if (reverb::voiceSend(m_selectedVoice)) {
        acidRom.m_font.print(acidRom.gpu(), "REV",
                            {{.x = 28, .y = 74}}, reverb::enabled() ? cyan : dim);
    }

    // Family / sub-label.
    const char *family =
        (m_selectedVoice <= 6)  ? "TR-808 ANALOG" :
        (m_selectedVoice <= 8)  ? "TB-303 STG 1"  :
        (m_selectedVoice <= 10) ? "TR-909 HYBRID" :
                                  "TB-303 STG 2";
    acidRom.m_font.print(acidRom.gpu(), family, {{.x = 100, .y = 40}}, vcol);
    acidRom.m_font.print(acidRom.gpu(), "VOICE", {{.x = 100, .y = 56}}, dim);
    // Voice index out of total.
    char vidx[8];
    vidx[0] = ' ';
    vidx[1] = '1' + static_cast<char>(m_selectedVoice / 10);
    if (m_selectedVoice >= 9) vidx[1] = '1', vidx[2] = '0' + static_cast<char>((m_selectedVoice + 1) % 10);
    else { vidx[1] = '0' + static_cast<char>(m_selectedVoice + 1); vidx[2] = ' '; }
    vidx[3] = '/'; vidx[4] = '1'; vidx[5] = '3'; vidx[6] = '\0';
    acidRom.m_font.print(acidRom.gpu(), vidx, {{.x = 100, .y = 72}}, white);

    // ============ Step row (16 big LEDs) ============
    constexpr int STEP_Y    = 108;
    constexpr int STEP_X0   = 16;
    constexpr int STEP_W    = 16;
    constexpr int STEP_H    = 26;
    constexpr int STEP_GAP  = 3;

    uint16_t pattern = m_voicePatterns[m_currentPattern][m_selectedVoice];
    for (int s = 0; s < NUM_STEPS; ++s) {
        int x = STEP_X0 + s * (STEP_W + STEP_GAP);
        bool active   = (pattern & (uint16_t(1) << s)) != 0;
        bool playing  = m_running && (s == m_playStep);
        bool isCursor = (s == m_cursorStep);

        psyqo::Color c = vcol;
        if (!active) { c.r >>= 3; c.g >>= 3; c.b >>= 3; }
        if (playing && active) {
            c.r = c.r > 200 ? 255 : c.r + 55;
            c.g = c.g > 200 ? 255 : c.g + 55;
            c.b = c.b > 200 ? 255 : c.b + 55;
        }
        psyqo::Prim::Rectangle led{c};
        led.position = {{.x = static_cast<int16_t>(x), .y = STEP_Y}};
        led.size     = {{.x = STEP_W, .y = STEP_H}};
        acidRom.gpu().sendPrimitive(led);

        // Quarter-beat marker — small bar above every 4th LED.
        if ((s & 3) == 0) {
            psyqo::Prim::Rectangle qb{cyan};
            qb.position = {{.x = static_cast<int16_t>(x + 6), .y = STEP_Y - 5}};
            qb.size     = {{.x = 4, .y = 2}};
            acidRom.gpu().sendPrimitive(qb);
        }

        // Cursor outline.
        if (isCursor) {
            psyqo::Color w{{.r = 255, .g = 255, .b = 255}};
            psyqo::Prim::Rectangle top{w}, bot{w}, lft{w}, rgt{w};
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

    // Step number ruler.
    for (int s = 0; s < NUM_STEPS; ++s) {
        char d[3];
        if (s < 9) { d[0] = '1' + static_cast<char>(s); d[1] = '\0'; }
        else { d[0] = '1'; d[1] = '0' + static_cast<char>((s + 1) % 10); d[2] = '\0'; }
        acidRom.m_font.print(acidRom.gpu(), d,
                            {{.x = static_cast<int16_t>(STEP_X0 + 4 + s * (STEP_W + STEP_GAP)),
                              .y = STEP_Y + STEP_H + 2}},
                            ((s & 3) == 0) ? amber : dim);
    }

    // ============ Voice picker strip (13 mini boxes) ============
    constexpr int PICK_Y = 168;
    constexpr int PICK_X0 = 8;
    constexpr int PICK_W  = 22;
    constexpr int PICK_H  = 18;
    constexpr int PICK_GAP = 1;
    for (int v = 0; v < NUM_VOICES; ++v) {
        int x = PICK_X0 + v * (PICK_W + PICK_GAP);
        psyqo::Color c = voiceColor(v);
        // Light any voice that has *any* step set in the current pattern.
        bool hasNotes = m_voicePatterns[m_currentPattern][v] != 0;
        if (!hasNotes) { c.r >>= 2; c.g >>= 2; c.b >>= 2; }
        psyqo::Prim::Rectangle box{c};
        box.position = {{.x = static_cast<int16_t>(x), .y = PICK_Y}};
        box.size     = {{.x = PICK_W, .y = PICK_H}};
        acidRom.gpu().sendPrimitive(box);
        // Selected voice gets a white outline.
        if (v == m_selectedVoice) {
            psyqo::Color w{{.r = 255, .g = 255, .b = 255}};
            psyqo::Prim::Rectangle top{w}, bot{w}, lft{w}, rgt{w};
            top.position = {{.x = static_cast<int16_t>(x - 1), .y = PICK_Y - 1}};
            top.size     = {{.x = PICK_W + 2, .y = 1}};
            bot.position = {{.x = static_cast<int16_t>(x - 1), .y = PICK_Y + PICK_H}};
            bot.size     = {{.x = PICK_W + 2, .y = 1}};
            lft.position = {{.x = static_cast<int16_t>(x - 1), .y = PICK_Y}};
            lft.size     = {{.x = 1, .y = PICK_H}};
            rgt.position = {{.x = static_cast<int16_t>(x + PICK_W), .y = PICK_Y}};
            rgt.size     = {{.x = 1, .y = PICK_H}};
            acidRom.gpu().sendPrimitive(top);
            acidRom.gpu().sendPrimitive(bot);
            acidRom.gpu().sendPrimitive(lft);
            acidRom.gpu().sendPrimitive(rgt);
        }
        // Label inside.
        acidRom.m_font.print(acidRom.gpu(), g_voices[v].name,
                            {{.x = static_cast<int16_t>(x + 2), .y = PICK_Y + 5}},
                            (v == m_selectedVoice) ? white : dim);
    }

    // Help footer.
    acidRom.m_font.print(acidRom.gpu(),
        "X:step  S/D:voice  L/R:pat  F:knob",
        {{.x = 16, .y = 200}}, dim);
    acidRom.m_font.print(acidRom.gpu(),
        "ENTER:play  A:rev  W:send  BS:chain",
        {{.x = 16, .y = 214}}, dim);
}

void SequencerScene::drawKnobPage() {
    psyqo::Color white{{.r = 240, .g = 240, .b = 240}};
    psyqo::Color cyan {{.r =  60, .g = 200, .b = 220}};
    psyqo::Color dim  {{.r =  90, .g = 100, .b = 115}};
    psyqo::Color vcol = voiceColor(m_selectedVoice);

    psyqo::Prim::Rectangle headerBg{{{.r = 24, .g = 28, .b = 36}}};
    headerBg.position = {{.x = 0, .y = 0}};
    headerBg.size     = {{.x = 320, .y = 22}};
    acidRom.gpu().sendPrimitive(headerBg);

    acidRom.m_font.print(acidRom.gpu(), g_voices[m_selectedVoice].name,
                        {{.x = 8, .y = 6}}, vcol);
    acidRom.m_font.print(acidRom.gpu(), "KNOBS", {{.x = 56, .y = 6}}, cyan);
    char buf[8];
    buf[0] = 'P'; buf[1] = 'A'; buf[2] = 'T'; buf[3] = ' ';
    buf[4] = '1' + static_cast<char>(m_currentPattern); buf[5] = '\0';
    acidRom.m_font.print(acidRom.gpu(), buf, {{.x = 132, .y = 6}}, white);
    acidRom.m_font.print(acidRom.gpu(),
        reverb::enabled() ? "REV" : "DRY",
        {{.x = 168, .y = 6}}, reverb::enabled() ? cyan : dim);
    acidRom.m_font.print(acidRom.gpu(),
        reverb::voiceSend(m_selectedVoice) ? "SEND" : "    ",
        {{.x = 200, .y = 6}}, reverb::enabled() ? cyan : dim);
    acidRom.m_font.print(acidRom.gpu(),
        m_running ? "PLAY" : "STOP", {{.x = 264, .y = 6}}, m_running ? cyan : dim);

    // Voice chooser strip — same 13-voice picker as the sequencer view,
    // along the left side stacked vertically? Actually keeping it horizontal
    // at the bottom mirrors the main view so the user's mental model stays.
    const RowKnobs &k = m_knobs[m_selectedVoice];
    auto drawFader = [&](int slot, const char *label, int value, int valueMax) {
        constexpr int FX0     = 36;
        constexpr int FY0     = 36;
        constexpr int F_W     = 50;
        constexpr int F_H     = 110;
        constexpr int F_STEP  = 64;
        int x = FX0 + slot * F_STEP;
        int y = FY0;

        psyqo::Color box{{.r = 14, .g = 18, .b = 25}};
        psyqo::Prim::Rectangle outer{box};
        outer.position = {{.x = static_cast<int16_t>(x), .y = static_cast<int16_t>(y)}};
        outer.size     = {{.x = F_W, .y = F_H}};
        acidRom.gpu().sendPrimitive(outer);

        if (slot == m_knobCursor) {
            psyqo::Prim::Rectangle bTop{cyan}, bBot{cyan}, bL{cyan}, bR{cyan};
            bTop.position = {{.x = static_cast<int16_t>(x - 1), .y = static_cast<int16_t>(y - 1)}};
            bTop.size     = {{.x = F_W + 2, .y = 1}};
            bBot.position = {{.x = static_cast<int16_t>(x - 1), .y = static_cast<int16_t>(y + F_H)}};
            bBot.size     = {{.x = F_W + 2, .y = 1}};
            bL.position   = {{.x = static_cast<int16_t>(x - 1), .y = static_cast<int16_t>(y)}};
            bL.size       = {{.x = 1, .y = F_H}};
            bR.position   = {{.x = static_cast<int16_t>(x + F_W), .y = static_cast<int16_t>(y)}};
            bR.size       = {{.x = 1, .y = F_H}};
            acidRom.gpu().sendPrimitive(bTop);
            acidRom.gpu().sendPrimitive(bBot);
            acidRom.gpu().sendPrimitive(bL);
            acidRom.gpu().sendPrimitive(bR);
        }

        int fillH = (F_H - 4) * value / valueMax;
        if (fillH < 0) fillH = 0;
        if (fillH > F_H - 4) fillH = F_H - 4;
        psyqo::Prim::Rectangle fill{vcol};
        fill.position = {{.x = static_cast<int16_t>(x + 2),
                          .y = static_cast<int16_t>(y + F_H - 2 - fillH)}};
        fill.size     = {{.x = F_W - 4, .y = static_cast<int16_t>(fillH)}};
        acidRom.gpu().sendPrimitive(fill);

        for (int t = 1; t <= 3; ++t) {
            psyqo::Prim::Rectangle tick{dim};
            tick.position = {{.x = static_cast<int16_t>(x + 2),
                              .y = static_cast<int16_t>(y + (F_H * t) / 4)}};
            tick.size     = {{.x = F_W - 4, .y = 1}};
            acidRom.gpu().sendPrimitive(tick);
        }

        acidRom.m_font.print(acidRom.gpu(), label,
                            {{.x = static_cast<int16_t>(x + 12),
                              .y = static_cast<int16_t>(y + F_H + 4)}}, cyan);
    };

    drawFader(0, "LVL", k.level, 255);
    drawFader(1, "PIT", k.pitch + 12, 24);
    drawFader(2, "TNE", k.tone, 255);
    drawFader(3, "DCY", k.decay, 255);

    auto printNum = [&](int slot, int val) {
        char b[5];
        if (val < 0) { b[0] = '-'; val = -val; } else b[0] = ' ';
        b[1] = '0' + ((val / 100) % 10);
        b[2] = '0' + ((val / 10)  % 10);
        b[3] = '0' + ( val        % 10);
        b[4] = '\0';
        acidRom.m_font.print(acidRom.gpu(), b,
                            {{.x = static_cast<int16_t>(54 + slot * 64),
                              .y = 164}}, white);
    };
    printNum(0, k.level);
    printNum(1, k.pitch);
    printNum(2, k.tone);
    printNum(3, k.decay);

    // Help.
    acidRom.m_font.print(acidRom.gpu(),
        "LR:knob  UD:voice  X/Z:+/-  F:back",
        {{.x = 16, .y = 200}}, dim);
    acidRom.m_font.print(acidRom.gpu(), "ps1-acid-rom",
        {{.x = 112, .y = 224}}, white);
}

void SequencerScene::frame() {
    psyqo::Color bg{{.r = 8, .g = 4, .b = 16}};
    acidRom.gpu().clear(bg);

    handleInput();
    advancePlayback();

    if (m_knobPage) drawKnobPage();
    else            drawSequencerPage();

    // M9 PSVJ sync stripe (top-right corner). Encodes step / pattern /
    // selected voice / reverb state in four 4x4 colored cells.
    auto stripeCell = [&](int idx, uint8_t r, uint8_t g, uint8_t b) {
        psyqo::Prim::Rectangle cell{{{.r = r, .g = g, .b = b}}};
        cell.position = {{.x = static_cast<int16_t>(304 + idx * 4), .y = 0}};
        cell.size     = {{.x = 4, .y = 4}};
        acidRom.gpu().sendPrimitive(cell);
    };
    stripeCell(0, static_cast<uint8_t>(m_playStep),
                  static_cast<uint8_t>(m_chainLength),
                  0x00);
    stripeCell(1, static_cast<uint8_t>(m_playingPattern),
                  static_cast<uint8_t>(m_currentPattern),
                  reverb::enabled() ? 1 : 0);
    stripeCell(2, static_cast<uint8_t>(m_selectedVoice),
                  m_running ? 0xff : 0x00,
                  0x00);
    stripeCell(3, 0x00, 0x00, 0x00);

    m_frameCounter++;
}

int main() { return acidRom.run(); }
