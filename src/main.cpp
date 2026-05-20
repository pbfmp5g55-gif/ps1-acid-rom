// ps1-acid-rom — PS1 sequencer inspired by ReBirth RB-338
//
// M0: hello-world style boot. Renders a static splash with the four
// instrument panels laid out; no sequencer, no audio yet. The goal of M0 is to
// prove the end-to-end toolchain: psyqo build → ps-exe / ISO → pcsx-redux boot.

#include "psyqo/application.hh"
#include "psyqo/font.hh"
#include "psyqo/gpu.hh"
#include "psyqo/scene.hh"

namespace {

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

    acidRom.m_font.print(acidRom.gpu(), "ps1-acid-rom M0", {{.x = 80, .y = 224}},
                         {{.r = 255, .g = 255, .b = 255}});
    m_tick++;
}

int main() { return acidRom.run(); }
