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

#include "common/hardware/spu.h"

#include "generated/voice_samples.h"

namespace {

constexpr uint16_t HALF_RATE = 0x0800;     // 22 050 Hz source → native pitch
constexpr uint32_t HOLD_ADSR = 0x1fff80ff; // hold-and-release envelope

constexpr int NUM_ROWS  = 4;
constexpr int NUM_STEPS = 16;
constexpr int FRAMES_PER_STEP = 8;          // ~112 BPM @ 60 Hz NTSC

constexpr int NUM_VOICES = 13;

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
    {"BD9", acid::voice_samples::r909bd_adpcm,         acid::voice_samples::r909bd_adpcm_bytes,         0x3000, 0},
    {"SD9", acid::voice_samples::r909sd_adpcm,         acid::voice_samples::r909sd_adpcm_bytes,         0x2400, 0},
    {"SW2", acid::voice_samples::tb303s2_saw_adpcm,    acid::voice_samples::tb303s2_saw_adpcm_bytes,    0x2800, 0},
    {"SQ2", acid::voice_samples::tb303s2_square_adpcm, acid::voice_samples::tb303s2_square_adpcm_bytes, 0x2400, 0},
};

constexpr uint8_t CH_PER_ROW[NUM_ROWS] = {0, 1, 2, 3};

// ----------------------------------------------------------------------------
// SPU reverb (M8) — Sony "Hall" preset. Reverb work area lives in the last
// 64 KB of SPU RAM (which is 512 KB total = 0x80000), so voices placed from
// 0x1100 upward (≤ ~130 KB) never collide with it.

namespace reverb {

constexpr uint32_t WORK_AREA_START_BYTES = 0x70000;          // = 448 KB

// 32 × u16 reverb config block at 0x1F801DC0..0x1F801DFE.
volatile uint16_t *const REVERB_REGS = reinterpret_cast<volatile uint16_t *>(0x1F801DC0);

// Sony Hall preset. Same values that appear in the well-known PSX SDK
// presets and no$psx documentation. Offsets are in 8-byte SPU units.
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

// Persistent state — toggled from the input handler.
bool g_enabled = false;
uint32_t g_channelMask = 0;  // per-voice reverb-send mask (bit N = channel N)

void setup() {
    // Halt reverb writes during reconfiguration (bit 7 of SPU_CTRL).
    SPU_CTRL = SPU_CTRL & ~(1u << 7);

    SPU_REVERB_ADDR = static_cast<uint16_t>(WORK_AREA_START_BYTES / 8);

    for (int i = 0; i < 32; ++i) REVERB_REGS[i] = HALL[i];

    // Reverb master volume — start centered, dial up after toggling on.
    SPU_REVERB_LEFT  = 0x3000;
    SPU_REVERB_RIGHT = 0x3000;

    SPU_REVERB_EN_LOW  = 0;
    SPU_REVERB_EN_HIGH = 0;

    // Re-enable reverb processor; main SPU enable stays untouched.
    SPU_CTRL = SPU_CTRL | (1u << 7);
}

void apply() {
    SPU_REVERB_EN_LOW  = g_enabled ? static_cast<uint16_t>( g_channelMask        & 0xFFFFu) : 0;
    SPU_REVERB_EN_HIGH = g_enabled ? static_cast<uint16_t>((g_channelMask >> 16) & 0xFFFFu) : 0;
}

void setRowSend(int row, bool on) {
    uint32_t bit = 1u << CH_PER_ROW[row];
    if (on) g_channelMask |=  bit;
    else    g_channelMask &= ~bit;
    apply();
}

void setEnabled(bool on) { g_enabled = on; apply(); }
bool enabled() { return g_enabled; }
bool rowSend(int row) { return (g_channelMask >> CH_PER_ROW[row]) & 1u; }

}  // namespace reverb

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

    // M4: up to 8 patterns in RAM. m_currentPattern is the one being edited
    // and displayed in the grid. m_chainLength controls how many patterns
    // cycle during playback (1 = current pattern loops; 2 = 0->1->0->1; up
    // to 8). m_playingPattern is which pattern is currently being heard
    // (== m_currentPattern when chainLength=1).
    static constexpr int NUM_PATTERNS = 8;
    uint16_t m_patterns[NUM_PATTERNS][NUM_ROWS] = {
        // Default seed pattern at slot 0. Others empty.
        {
            0b0010001000100010,
            0b0000000000000000,
            0b1000100010001000,
            0b0101010101010101,
        },
    };
    int m_currentPattern  = 0;
    int m_playingPattern  = 0;
    int m_chainLength     = 1;
    int m_chainPos        = 0;

    // Voice mapping is global across patterns (drum machine convention —
    // sound stays put when you switch patterns).
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

    if (press(B::Cross))  m_patterns[m_currentPattern][m_cursorRow] ^= (uint16_t(1) << m_cursorStep);
    if (press(B::Square)) m_patterns[m_currentPattern][m_cursorRow] = 0;
    if (press(B::Start))  m_running = !m_running;

    // Triangle: next voice for cursor row. Circle: previous.
    if (press(B::Triangle)) {
        m_voiceIdx[m_cursorRow] = (m_voiceIdx[m_cursorRow] + 1) % NUM_VOICES;
    }
    if (press(B::Circle)) {
        m_voiceIdx[m_cursorRow] = (m_voiceIdx[m_cursorRow] + NUM_VOICES - 1) % NUM_VOICES;
    }

    // L1 / R1: cycle the pattern being edited (0..7).
    if (press(B::L1)) m_currentPattern = (m_currentPattern + NUM_PATTERNS - 1) % NUM_PATTERNS;
    if (press(B::R1)) m_currentPattern = (m_currentPattern + 1) % NUM_PATTERNS;

    // Select: cycle chain length 1..NUM_PATTERNS. At length 1 only the
    // current pattern loops; at higher lengths patterns 0..length-1 cycle.
    if (press(B::Select)) {
        m_chainLength = (m_chainLength % NUM_PATTERNS) + 1;
        m_chainPos = 0;
    }

    // L2: toggle global reverb on/off.
    if (press(B::L2)) reverb::setEnabled(!reverb::enabled());
    // L3: toggle reverb send for the cursor row.
    if (press(B::L3)) reverb::setRowSend(m_cursorRow, !reverb::rowSend(m_cursorRow));

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

    int newStep = (m_frameCounter / FRAMES_PER_STEP) % NUM_STEPS;
    // Detect bar wraparound (step 15 → 0) and advance the chain position.
    if (newStep == 0 && m_playStep != 0) {
        if (m_chainLength > 1) {
            m_chainPos = (m_chainPos + 1) % m_chainLength;
        }
    }
    m_playStep = newStep;

    m_playingPattern = (m_chainLength > 1) ? m_chainPos : m_currentPattern;

    for (int row = 0; row < NUM_ROWS; ++row) {
        if (m_patterns[m_playingPattern][row] & (uint16_t(1) << m_playStep)) {
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

    // Reverb send indicator — small "R" badge after voice name when this
    // row is being sent to the SPU reverb.
    if (reverb::rowSend(row)) {
        psyqo::Color rCol = reverb::enabled()
            ? psyqo::Color{{.r = 120, .g = 200, .b = 255}}
            : psyqo::Color{{.r = 80,  .g = 80,  .b = 80}};
        acidRom.m_font.print(acidRom.gpu(), "R",
                            {{.x = 40, .y = static_cast<int16_t>(y + 18)}},
                            rCol);
    }

    if (row == m_cursorRow) {
        psyqo::Prim::Rectangle bar{{{.r = 240, .g = 240, .b = 240}}};
        bar.position = {{.x = 48, .y = static_cast<int16_t>(y)}};
        bar.size     = {{.x = 2,  .y = static_cast<int16_t>(LED_H + 8)}};
        acidRom.gpu().sendPrimitive(bar);
    }

    for (int step = 0; step < NUM_STEPS; ++step) {
        int x = LEDS_X0 + step * (LED_W + LED_GAP);
        bool active   = (m_patterns[m_currentPattern][row] & (uint16_t(1) << step)) != 0;
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
    psyqo::Color amber{{.r = 240, .g = 180, .b = 60}};

    acidRom.m_font.print(acidRom.gpu(),
        m_running ? "PLAY" : "STOP", {{.x = 8, .y = 168}},
        m_running ? white : dim);

    // Pattern / chain readout: "PAT 1/8  CHN 4  PLAY 2"
    // PAT = pattern being edited.  CHN = chain length.  PLAY = which
    // pattern is currently audible (only meaningful when CHN > 1).
    char patBuf[8];
    patBuf[0] = 'P'; patBuf[1] = 'A'; patBuf[2] = 'T'; patBuf[3] = ' ';
    patBuf[4] = '1' + static_cast<char>(m_currentPattern); patBuf[5] = '/';
    patBuf[6] = '0' + static_cast<char>(NUM_PATTERNS);
    patBuf[7] = '\0';
    acidRom.m_font.print(acidRom.gpu(), patBuf, {{.x = 56, .y = 168}}, amber);

    char chnBuf[8];
    chnBuf[0] = 'C'; chnBuf[1] = 'H'; chnBuf[2] = 'N'; chnBuf[3] = ' ';
    chnBuf[4] = '1' + static_cast<char>(m_chainLength - 1);
    chnBuf[5] = '\0';
    acidRom.m_font.print(acidRom.gpu(), chnBuf, {{.x = 128, .y = 168}}, white);

    if (m_chainLength > 1) {
        char nowBuf[8];
        nowBuf[0] = '>'; nowBuf[1] = ' ';
        nowBuf[2] = '1' + static_cast<char>(m_playingPattern);
        nowBuf[3] = '\0';
        acidRom.m_font.print(acidRom.gpu(), nowBuf, {{.x = 176, .y = 168}}, white);
    }

    // Reverb state.
    psyqo::Color revCol = reverb::enabled()
        ? psyqo::Color{{.r = 120, .g = 200, .b = 255}}
        : dim;
    acidRom.m_font.print(acidRom.gpu(),
        reverb::enabled() ? "REV ON" : "REV OFF",
        {{.x = 232, .y = 168}}, revCol);

    acidRom.m_font.print(acidRom.gpu(),
                        "X:tgl TRI/O:voice L/R:pat SEL:chn L2:rev L3:send",
                        {{.x = 4, .y = 200}}, dim);
    acidRom.m_font.print(acidRom.gpu(), "ps1-acid-rom",
                        {{.x = 112, .y = 224}}, white);
}

void SequencerScene::frame() {
    psyqo::Color bg{{.r = 8, .g = 4, .b = 16}};
    acidRom.gpu().clear(bg);

    handleInput();
    advancePlayback();

    for (int row = 0; row < NUM_ROWS; ++row) drawRow(row);
    drawStatus();

    // M9: PSVJ sync stripe in the top-right corner. Four 4x4 rectangles
    // encoding the current sequencer state in their RGB triplets, so the
    // ps1-vj-mix mixer can lock VJ params (chance / chaos / texture swap)
    // to whatever pattern is currently playing. Contract documented in
    // design/PSVJ_INTEGRATION.md; mixer side TODO.
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
    stripeCell(2, static_cast<uint8_t>(m_voiceIdx[0]),
                  static_cast<uint8_t>(m_voiceIdx[1]),
                  static_cast<uint8_t>(m_voiceIdx[2]));
    stripeCell(3, static_cast<uint8_t>(m_voiceIdx[3]),
                  m_running ? 0xff : 0x00,
                  0x00);

    m_frameCounter++;
}

int main() { return acidRom.run(); }
