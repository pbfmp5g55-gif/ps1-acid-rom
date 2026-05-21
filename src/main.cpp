// ps1-acid-rom — PS1 sequencer inspired by ReBirth RB-338.
//
// Current milestone slice: M2 (SPU integration).
//   At boot we upload three pre-rendered voice samples (BD / HH / TB-303,
//   generated host-side from src/dsp/*.cpp via host_tests/gen_voice_samples)
//   to SPU RAM, then play a small repeating pattern from the splash scene's
//   frame() callback. The DSP voices themselves are NOT linked into the PS1
//   build — that requires libm and is deferred. The samples *were* generated
//   by the exact same C++ source running host-side, so what plays on the PS1
//   is bit-identical to the .wav files we judge by ear.
//
//   M0 splash UI (4 placeholder panels) is preserved underneath so we keep
//   visual confirmation that the GPU loop is alive while sound plays.

#include "psyqo/application.hh"
#include "psyqo/font.hh"
#include "psyqo/gpu.hh"
#include "psyqo/scene.hh"
#include "psyqo/spu.hh"

#include "generated/voice_samples.h"

namespace {

// SPU RAM layout. Addresses are byte-offsets; the SPU registers want them
// divided by 8 (psyqo::SPU helpers handle that). 0x1000 holds psyqo's
// silent dummy used as the safe loop target, so we put our payloads above.
constexpr uint32_t SPU_BASE        = 0x1100;
constexpr uint32_t BD_SPU_ADDR     = SPU_BASE;
constexpr uint32_t HH_SPU_ADDR     = BD_SPU_ADDR + acid::voice_samples::bd_adpcm_bytes;
constexpr uint32_t TB303_SPU_ADDR  = HH_SPU_ADDR + acid::voice_samples::hh_adpcm_bytes;

// Voice samples were rendered at 22 050 Hz on the host. SPU's sampleRate
// register treats 0x1000 as "native" (44 100 Hz), so 0x0800 = half = play
// our samples back at their original pitch.
constexpr uint16_t HALF_RATE = 0x0800;

// Per-voice SPU channels. Channel 0/1/2 = BD/HH/TB303 — fixed, low IDs are
// fine because we don't share the SPU with anything else yet.
constexpr uint8_t CH_BD    = 0;
constexpr uint8_t CH_HH    = 1;
constexpr uint8_t CH_TB303 = 2;

// ADSR: full-on instant attack, near-instant decay, no sustain envelope —
// our samples already carry the envelope shape we baked in host-side.
// Lower 16 bits = AD register, upper = SR.
//   AD:   attack mode/rate (we want fastest), decay rate (slowest = hold).
//   SR:   sustain level (max), release mode/rate (fastest = stop on key off).
// We want the sample to play through to its natural end, so make ADSR a
// "hold until sample finishes" curve. Concrete: AD = 0x80ff (fast attack,
// long decay), SR = 0x1fff (max sustain level, slow release).
constexpr uint32_t HOLD_ADSR = 0x1fff80ff;

void uploadVoice(uint32_t spuAddr, const uint8_t *data, unsigned bytes) {
    // SPU DMA block size must divide the payload size. 16 bytes (= 1 ADPCM
    // block) divides everything we feed it.
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
};

class SplashScene final : public psyqo::Scene {
    void frame() override;
    uint32_t m_tick = 0;
};

AcidRom acidRom;
SplashScene splashScene;

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
    m_font.uploadSystemFont(gpu());
    pushScene(&splashScene);
}

void SplashScene::frame() {
    psyqo::Color bg{{.r = 8, .g = 4, .b = 16}};
    acidRom.gpu().clear(bg);

    // 4 instrument panel placeholders (303a / 303b / 808 / 909) laid out 2x2.
    // M1 replaces these with real panel art.
    struct PanelRect {
        int16_t x, y, w, h;
        psyqo::Color col;
        const char *label;
    };
    PanelRect panels[4] = {
        {  8,  16, 144, 88, {{.r = 200, .g = 50,  .b = 30 }}, "303 A"},
        {160,  16, 144, 88, {{.r = 200, .g = 130, .b = 30 }}, "303 B"},
        {  8, 112, 144, 88, {{.r = 50,  .g = 50,  .b = 200}}, "808  "},
        {160, 112, 144, 88, {{.r = 50,  .g = 150, .b = 100}}, "909  "},
    };

    for (auto &p : panels) {
        // Pulse brightness with the tick so we can see the frame loop running.
        uint8_t k = uint8_t((m_tick + (&p - panels) * 64) & 0xFF);
        psyqo::Color c = p.col;
        c.r = uint8_t((int(c.r) * k) / 255);
        c.g = uint8_t((int(c.g) * k) / 255);
        c.b = uint8_t((int(c.b) * k) / 255);
        acidRom.m_font.print(acidRom.gpu(), p.label, {{.x = p.x + 8, .y = p.y + 8}}, c);
    }

    // Tiny demo pattern at ~120 BPM on a NTSC (60 Hz) timeline:
    //   - BD every 16 frames    (≈ quarter note)
    //   - HH every 8  frames    (≈ eighth)
    //   - TB-303 every 32 frames (≈ half) with alternating velocity
    // Once M1 / M2-live land this gets replaced with a real step sequencer.
    if ((m_tick & 0x0F) == 0) triggerVoice(CH_BD,    BD_SPU_ADDR,    0x3000);
    if ((m_tick & 0x07) == 0) triggerVoice(CH_HH,    HH_SPU_ADDR,    0x1800);
    if ((m_tick & 0x1F) == 0) {
        uint16_t vol = (m_tick & 0x20) ? 0x2800 : 0x1c00;
        triggerVoice(CH_TB303, TB303_SPU_ADDR, vol);
    }

    acidRom.m_font.print(acidRom.gpu(), "ps1-acid-rom M2", {{.x = 80, .y = 224}},
                         {{.r = 255, .g = 255, .b = 255}});
    m_tick++;
}

int main() { return acidRom.run(); }
