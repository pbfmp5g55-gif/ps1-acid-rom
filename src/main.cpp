// ps1-acid-rom — PS1 sequencer inspired by ReBirth RB-338.
//
// Current milestone slice: M1 (UI skeleton) over M2 (SPU integration).
//   - Four voice rows (303 A / 303 B / 808 / 909) stacked vertically.
//   - Each row carries a 16-step grid drawn as small LED rectangles.
//   - Pad navigates the cursor (D-pad), Cross toggles the current step,
//     Square clears the selected row, Start toggles play/pause, Triangle
//     swaps the BD/HH/TB-303 sample assignment as a quick variant knob.
//   - During playback (~112 BPM at 60 Hz NTSC, 8 frames per step), active
//     steps trigger the SPU samples uploaded by M2.
//
// Until M5 / M7 land properly:
//   - 303A and 303B both fire the TB-303 sample (no per-row pitch yet —
//     deferred to M2-live or M5b).
//   - 808 fires BD, 909 fires HH. CY/CP/CB/SD/Tom are pre-rendered into
//     samples but not bound to UI rows yet.
//
// DSP voices themselves are not linked into the PS1 build (no libm); the
// samples were generated bit-exact host-side by host_tests/gen_voice_samples.

#include "psyqo/application.hh"
#include "psyqo/font.hh"
#include "psyqo/gpu.hh"
#include "psyqo/primitives/rectangles.hh"
#include "psyqo/scene.hh"
#include "psyqo/simplepad.hh"
#include "psyqo/spu.hh"

#include "generated/voice_samples.h"

namespace {

// SPU RAM layout — addresses are byte-offsets; 0x1000 is the silent dummy.
constexpr uint32_t SPU_BASE       = 0x1100;
constexpr uint32_t BD_SPU_ADDR    = SPU_BASE;
constexpr uint32_t HH_SPU_ADDR    = BD_SPU_ADDR + acid::voice_samples::bd_adpcm_bytes;
constexpr uint32_t TB303_SPU_ADDR = HH_SPU_ADDR + acid::voice_samples::hh_adpcm_bytes;

constexpr uint16_t HALF_RATE = 0x0800;     // 22 050 Hz source → native pitch
constexpr uint32_t HOLD_ADSR = 0x1fff80ff; // hold-and-release envelope

// One SPU channel per row keeps voices independent (303A and 303B will
// drift apart once we add per-row pitch).
constexpr uint8_t CH_PER_ROW[4] = {0, 1, 2, 3};

// Per-row sample address and volume. Index matches m_pattern row.
struct RowVoice {
    uint32_t spuAddr;
    uint16_t volume;
};
constexpr RowVoice ROW_VOICES[4] = {
    {TB303_SPU_ADDR, 0x2800},  // 303 A
    {TB303_SPU_ADDR, 0x2800},  // 303 B (same sample for now)
    {BD_SPU_ADDR,    0x3000},  // 808 BD
    {HH_SPU_ADDR,    0x1800},  // 909 HH
};

constexpr int FRAMES_PER_STEP = 8;  // ~112 BPM @ 60 Hz NTSC, 16th notes
constexpr int NUM_ROWS        = 4;
constexpr int NUM_STEPS       = 16;

// Layout constants (320 x 240, NTSC).
constexpr int ROW_HEIGHT = 32;
constexpr int ROW_Y0     = 24;
constexpr int LED_W      = 12;
constexpr int LED_H      = 12;
constexpr int LED_GAP    = 2;
constexpr int LEDS_X0    = 64;

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

void triggerVoice(uint8_t channel, uint32_t spuAddr, uint16_t volume) {
    psyqo::SPU::ChannelPlaybackConfig cfg{};
    cfg.sampleRate.value = HALF_RATE;
    cfg.volumeLeft  = volume;
    cfg.volumeRight = volume;
    cfg.adsr = HOLD_ADSR;
    psyqo::SPU::playADPCM(channel, static_cast<uint16_t>(spuAddr), cfg, true);
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
        // Default seed pattern so first boot has something audible.
        0b0010001000100010,  // 303A: off-beats
        0b0000000000000000,  // 303B: empty
        0b1000100010001000,  // 808 BD: 4-on-the-floor (LSB = step 0)
        0b0101010101010101,  // 909 HH: every off-eighth
    };
    int m_cursorRow  = 2;
    int m_cursorStep = 0;
    int m_playStep   = 0;
    uint32_t m_frameCounter = 0;
    bool m_running = true;

    // Edge-detect: which buttons were down on the previous frame.
    uint16_t m_prevButtons[2] = {0, 0};
};

AcidRom acidRom;
SequencerScene sequencerScene;

bool justPressed(psyqo::SimplePad::Button b, uint16_t prev) {
    // SimplePad's m_padData uses inverted bits; isButtonPressed says
    // (m_padData[1] & (1<<b)) == 0 means pressed. We mirror that.
    bool nowPressed = acidRom.m_input.isButtonPressed(psyqo::SimplePad::Pad1, b);
    bool wasPressed = (prev & (1 << b)) == 0;
    return nowPressed && !wasPressed;
}

}  // namespace

void AcidRom::prepare() {
    psyqo::GPU::Configuration config;
    config.set(psyqo::GPU::Resolution::W320)
        .set(psyqo::GPU::VideoMode::AUTO)
        .set(psyqo::GPU::ColorMode::C15BITS)
        .set(psyqo::GPU::Interlace::PROGRESSIVE);
    gpu().initialize(config);

    psyqo::SPU::initialize();
    uploadVoice(BD_SPU_ADDR,    acid::voice_samples::bd_adpcm,
                acid::voice_samples::bd_adpcm_bytes);
    uploadVoice(HH_SPU_ADDR,    acid::voice_samples::hh_adpcm,
                acid::voice_samples::hh_adpcm_bytes);
    uploadVoice(TB303_SPU_ADDR, acid::voice_samples::tb303_adpcm,
                acid::voice_samples::tb303_adpcm_bytes);
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

    // Capture button bitmap for next-frame edge detection.
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
            triggerVoice(CH_PER_ROW[row],
                         ROW_VOICES[row].spuAddr,
                         ROW_VOICES[row].volume);
        }
    }
}

void SequencerScene::drawRow(int row) {
    int y = ROW_Y0 + row * ROW_HEIGHT;

    // Row label.
    acidRom.m_font.print(acidRom.gpu(), ROW_LABELS[row],
                        {{.x = 8, .y = static_cast<int16_t>(y + 2)}},
                        ROW_COLORS[row]);

    // Cursor row gets a thin highlight bar to its left.
    if (row == m_cursorRow) {
        psyqo::Prim::Rectangle bar{{{.r = 240, .g = 240, .b = 240}}};
        bar.position = {{.x = 56, .y = static_cast<int16_t>(y)}};
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
            // Dim background — keep the row's hue but darken heavily.
            c.r = c.r >> 3;
            c.g = c.g >> 3;
            c.b = c.b >> 3;
        }
        if (playing && active) {
            // Boost — clamp to 0xFF.
            c.r = c.r > 200 ? 255 : c.r + 55;
            c.g = c.g > 200 ? 255 : c.g + 55;
            c.b = c.b > 200 ? 255 : c.b + 55;
        }

        psyqo::Prim::Rectangle led{c};
        led.position = {{.x = static_cast<int16_t>(x),     .y = static_cast<int16_t>(y + 4)}};
        led.size     = {{.x = LED_W, .y = LED_H}};
        acidRom.gpu().sendPrimitive(led);

        // Cursor outline (white) around the active cursor cell. We draw four
        // thin rectangles forming the border so we don't need a hollow-rect
        // primitive.
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
    acidRom.m_font.print(acidRom.gpu(), "X:toggle SQ:clear ST:play",
                        {{.x = 64, .y = 200}}, dim);
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
