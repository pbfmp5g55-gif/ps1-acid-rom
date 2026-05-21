// ps1-acid-rom — PS1 sequencer inspired by ReBirth RB-338.
//
// State of the build:
//   - 4 voice rows (303 A / 303 B / 808 / 909) × 16 step grid, drawn with
//     Prim::Rectangle LEDs.
//   - Each row holds an index into a 9-voice sample table; Triangle / Circle
//     cycle through the voices for the row under the cursor. Default mapping
//     is 303 A → TB-303 Saw, 303 B → TB-303 Square, 808 → BD, 909 → HH, but
//     anything goes — drop TB-303 on the 909 row if you want.
//   - SimplePad pad 1: D-pad cursor, Cross = step toggle, Square = clear row,
//     Start = play/pause, Triangle / Circle = next / prev voice for the
//     cursor row.
//   - All voice samples were rendered host-side by host_tests/gen_voice_samples
//     (same C++ DSP source as the host tests) and embedded as PSX ADPCM in
//     src/generated/voice_samples.h. The PS1 build does not link the DSP
//     voices directly because we don't pull in libm; that's deferred to
//     M2-live.

#include "psyqo/application.hh"
#include "psyqo/font.hh"
#include "psyqo/gpu.hh"
#include "psyqo/primitives/rectangles.hh"
#include "psyqo/scene.hh"
#include "psyqo/simplepad.hh"
#include "psyqo/spu.hh"

#include "generated/voice_samples.h"

namespace {

constexpr uint16_t HALF_RATE = 0x0800;     // 22 050 Hz source → native pitch
constexpr uint32_t HOLD_ADSR = 0x1fff80ff; // hold-and-release envelope

constexpr int NUM_ROWS  = 4;
constexpr int NUM_STEPS = 16;
constexpr int FRAMES_PER_STEP = 8;          // ~112 BPM @ 60 Hz NTSC

constexpr int NUM_VOICES = 9;

struct VoiceDef {
    const char *name;        // ≤3 chars for inline label
    const uint8_t *data;
    unsigned bytes;
    uint16_t volume;
    uint32_t spuAddr;        // computed at boot from running total
};

VoiceDef g_voices[NUM_VOICES] = {
    {"BD",  acid::voice_samples::bd_adpcm,           acid::voice_samples::bd_adpcm_bytes,           0x3000, 0},
    {"SD",  acid::voice_samples::sd_adpcm,           acid::voice_samples::sd_adpcm_bytes,           0x2400, 0},
    {"TOM", acid::voice_samples::tom_adpcm,          acid::voice_samples::tom_adpcm_bytes,          0x2800, 0},
    {"HH",  acid::voice_samples::hh_adpcm,           acid::voice_samples::hh_adpcm_bytes,           0x1800, 0},
    {"CY",  acid::voice_samples::cy_adpcm,           acid::voice_samples::cy_adpcm_bytes,           0x1c00, 0},
    {"CP",  acid::voice_samples::cp_adpcm,           acid::voice_samples::cp_adpcm_bytes,           0x2000, 0},
    {"CB",  acid::voice_samples::cb_adpcm,           acid::voice_samples::cb_adpcm_bytes,           0x1c00, 0},
    {"SAW", acid::voice_samples::tb303_saw_adpcm,    acid::voice_samples::tb303_saw_adpcm_bytes,    0x2800, 0},
    {"SQR", acid::voice_samples::tb303_square_adpcm, acid::voice_samples::tb303_square_adpcm_bytes, 0x2400, 0},
};

constexpr uint8_t CH_PER_ROW[NUM_ROWS] = {0, 1, 2, 3};

// Layout constants (320 x 240, NTSC). LED row spans x = LEDS_X0 .. LEDS_END.
constexpr int ROW_HEIGHT = 32;
constexpr int ROW_Y0     = 24;
constexpr int LED_W      = 12;
constexpr int LED_H      = 12;
constexpr int LED_GAP    = 2;
constexpr int LEDS_X0    = 56;
constexpr int LEDS_END   = LEDS_X0 + NUM_STEPS * (LED_W + LED_GAP);  // = 56 + 224 = 280

constexpr psyqo::Color ROW_COLORS[NUM_ROWS] = {
    {{.r = 200, .g = 50,  .b = 30 }},  // 303 A — orange-red
    {{.r = 200, .g = 130, .b = 30 }},  // 303 B — amber
    {{.r = 50,  .g = 50,  .b = 200}},  // 808   — blue
    {{.r = 50,  .g = 150, .b = 100}},  // 909   — teal-green
};
constexpr const char *ROW_LABELS[NUM_ROWS] = {"303A", "303B", "808 ", "909 "};

void uploadVoice(uint32_t spuAddr, const uint8_t *data, unsigned bytes) {
    psyqo::SPU::dmaWrite(spuAddr, data, static_cast<uint16_t>(bytes), 16);
}

void triggerVoice(uint8_t channel, const VoiceDef &v) {
    psyqo::SPU::ChannelPlaybackConfig cfg{};
    cfg.sampleRate.value = HALF_RATE;
    cfg.volumeLeft  = v.volume;
    cfg.volumeRight = v.volume;
    cfg.adsr = HOLD_ADSR;
    psyqo::SPU::playADPCM(channel, static_cast<uint16_t>(v.spuAddr), cfg, true);
}

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

    void drawRow(int row);
    void drawStatus();
    void handleInput();
    void advancePlayback();

    uint16_t m_pattern[NUM_ROWS] = {
        0b0010001000100010,  // 303 A
        0b0000000000000000,  // 303 B
        0b1000100010001000,  // 808 → BD: 4-on-the-floor (LSB = step 0)
        0b0101010101010101,  // 909 → HH: off-eighths
    };

    // Default voice mapping. Players can change via Triangle/Circle.
    int m_voiceIdx[NUM_ROWS] = {7, 8, 0, 3};  // SAW, SQR, BD, HH

    int m_cursorRow  = 2;
    int m_cursorStep = 0;
    int m_playStep   = 0;
    uint32_t m_frameCounter = 0;
    bool m_running = true;

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

    // Lay out all voice samples linearly in SPU RAM starting at 0x1100
    // (0x1000 is psyqo's silent dummy loop target). Compute addresses as
    // we go so adding a voice or resizing one doesn't need manual edits.
    uint32_t cursor = 0x1100;
    for (auto &v : g_voices) {
        v.spuAddr = cursor;
        uploadVoice(v.spuAddr, v.data, v.bytes);
        cursor += v.bytes;
    }
}

void AcidRom::createScene() {
    if (!m_initialized) {
        m_font.uploadSystemFont(gpu());
        m_input.initialize();
        m_initialized = true;
    }
    pushScene(&sequencerScene);
}

void SequencerScene::start(StartReason) {}

void SequencerScene::handleInput() {
    using B = psyqo::SimplePad::Button;
    auto &pad = acidRom.m_input;
    auto press = [&](B b) {
        bool now = pad.isButtonPressed(psyqo::SimplePad::Pad1, b);
        bool wasDown = (m_prevButtons[0] & (1 << b)) == 0;
        return now && !wasDown;
    };

    if (press(B::Left))  m_cursorStep = (m_cursorStep + NUM_STEPS - 1) % NUM_STEPS;
    if (press(B::Right)) m_cursorStep = (m_cursorStep + 1) % NUM_STEPS;
    if (press(B::Up))    m_cursorRow  = (m_cursorRow + NUM_ROWS - 1) % NUM_ROWS;
    if (press(B::Down))  m_cursorRow  = (m_cursorRow + 1) % NUM_ROWS;

    if (press(B::Cross))  m_pattern[m_cursorRow] ^= (uint16_t(1) << m_cursorStep);
    if (press(B::Square)) m_pattern[m_cursorRow] = 0;
    if (press(B::Start))  m_running = !m_running;

    // Triangle: next voice for cursor row. Circle: previous.
    if (press(B::Triangle)) {
        m_voiceIdx[m_cursorRow] = (m_voiceIdx[m_cursorRow] + 1) % NUM_VOICES;
    }
    if (press(B::Circle)) {
        m_voiceIdx[m_cursorRow] = (m_voiceIdx[m_cursorRow] + NUM_VOICES - 1) % NUM_VOICES;
    }

    uint16_t bits = 0;
    for (int b = 0; b < 16; ++b) {
        if (!pad.isButtonPressed(psyqo::SimplePad::Pad1, static_cast<B>(b))) {
            bits |= (1 << b);
        }
    }
    m_prevButtons[0] = bits;
}

void SequencerScene::advancePlayback() {
    if (!m_running) return;
    if ((m_frameCounter % FRAMES_PER_STEP) != 0) return;

    m_playStep = (m_frameCounter / FRAMES_PER_STEP) % NUM_STEPS;
    for (int row = 0; row < NUM_ROWS; ++row) {
        if (m_pattern[row] & (uint16_t(1) << m_playStep)) {
            triggerVoice(CH_PER_ROW[row], g_voices[m_voiceIdx[row]]);
        }
    }
}

void SequencerScene::drawRow(int row) {
    int y = ROW_Y0 + row * ROW_HEIGHT;

    // Row label.
    acidRom.m_font.print(acidRom.gpu(), ROW_LABELS[row],
                        {{.x = 8, .y = static_cast<int16_t>(y + 2)}},
                        ROW_COLORS[row]);

    // Voice name (3-char abbrev) under the row label.
    psyqo::Color voiceCol = ROW_COLORS[row];
    if (row != m_cursorRow) {
        voiceCol.r >>= 1;
        voiceCol.g >>= 1;
        voiceCol.b >>= 1;
    }
    acidRom.m_font.print(acidRom.gpu(), g_voices[m_voiceIdx[row]].name,
                        {{.x = 8, .y = static_cast<int16_t>(y + 18)}},
                        voiceCol);

    if (row == m_cursorRow) {
        psyqo::Prim::Rectangle bar{{{.r = 240, .g = 240, .b = 240}}};
        bar.position = {{.x = 48, .y = static_cast<int16_t>(y)}};
        bar.size     = {{.x = 2,  .y = static_cast<int16_t>(LED_H + 8)}};
        acidRom.gpu().sendPrimitive(bar);
    }

    for (int step = 0; step < NUM_STEPS; ++step) {
        int x = LEDS_X0 + step * (LED_W + LED_GAP);
        bool active   = (m_pattern[row] & (uint16_t(1) << step)) != 0;
        bool playing  = m_running && (step == m_playStep);
        bool isCursor = (row == m_cursorRow && step == m_cursorStep);

        psyqo::Color c = ROW_COLORS[row];
        if (!active) {
            c.r >>= 3;
            c.g >>= 3;
            c.b >>= 3;
        }
        if (playing && active) {
            c.r = c.r > 200 ? 255 : c.r + 55;
            c.g = c.g > 200 ? 255 : c.g + 55;
            c.b = c.b > 200 ? 255 : c.b + 55;
        }

        psyqo::Prim::Rectangle led{c};
        led.position = {{.x = static_cast<int16_t>(x),     .y = static_cast<int16_t>(y + 4)}};
        led.size     = {{.x = LED_W, .y = LED_H}};
        acidRom.gpu().sendPrimitive(led);

        if (isCursor) {
            psyqo::Color w{{.r = 255, .g = 255, .b = 255}};
            psyqo::Prim::Rectangle top{w}, bot{w}, lft{w}, rgt{w};
            top.position = {{.x = static_cast<int16_t>(x - 1),       .y = static_cast<int16_t>(y + 3)}};
            top.size     = {{.x = static_cast<int16_t>(LED_W + 2),   .y = 1}};
            bot.position = {{.x = static_cast<int16_t>(x - 1),       .y = static_cast<int16_t>(y + 4 + LED_H)}};
            bot.size     = {{.x = static_cast<int16_t>(LED_W + 2),   .y = 1}};
            lft.position = {{.x = static_cast<int16_t>(x - 1),       .y = static_cast<int16_t>(y + 4)}};
            lft.size     = {{.x = 1, .y = LED_H}};
            rgt.position = {{.x = static_cast<int16_t>(x + LED_W),   .y = static_cast<int16_t>(y + 4)}};
            rgt.size     = {{.x = 1, .y = LED_H}};
            acidRom.gpu().sendPrimitive(top);
            acidRom.gpu().sendPrimitive(bot);
            acidRom.gpu().sendPrimitive(lft);
            acidRom.gpu().sendPrimitive(rgt);
        }
    }
}

void SequencerScene::drawStatus() {
    psyqo::Color white{{.r = 255, .g = 255, .b = 255}};
    psyqo::Color dim  {{.r = 128, .g = 128, .b = 128}};
    acidRom.m_font.print(acidRom.gpu(),
        m_running ? "PLAY" : "STOP", {{.x = 8, .y = 168}},
        m_running ? white : dim);
    acidRom.m_font.print(acidRom.gpu(), "ps1-acid-rom M1",
                        {{.x = 80, .y = 224}}, white);
    acidRom.m_font.print(acidRom.gpu(),
                        "X:tgl SQ:clr ST:run TRI/O:voice",
                        {{.x = 32, .y = 200}}, dim);
}

void SequencerScene::frame() {
    psyqo::Color bg{{.r = 8, .g = 4, .b = 16}};
    acidRom.gpu().clear(bg);

    handleInput();
    advancePlayback();

    for (int row = 0; row < NUM_ROWS; ++row) drawRow(row);
    drawStatus();

    m_frameCounter++;
}

int main() { return acidRom.run(); }
