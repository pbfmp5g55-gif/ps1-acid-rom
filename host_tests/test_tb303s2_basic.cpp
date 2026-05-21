// Sanity tests for TB-303 stage 2 (Moog ladder).

#include "dsp/tb303_stage2.hpp"

#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
            failures++;                                                             \
        }                                                                           \
    } while (0)

int main() {
    constexpr int SR = 22050;

    {
        acid::dsp::TB303Stage2 v(SR);
        for (int i = 0; i < 200; ++i) CHECK(v.tick() == 0);
        CHECK(!v.isActive());
    }

    {  // audible after noteOn
        acid::dsp::TB303Stage2 v(SR);
        v.noteOn(110.0f, false, false);
        bool nz = false;
        int peak = 0;
        for (int i = 0; i < SR / 10; ++i) {
            int s = v.tick();
            if (s != 0) nz = true;
            if (std::abs(s) > peak) peak = std::abs(s);
        }
        CHECK(nz);
        CHECK(peak > 1000);
        CHECK(peak < 30000);
    }

    {  // accent louder than non-accent
        auto peakFor = [](bool accent) {
            acid::dsp::TB303Stage2 v(SR);
            v.noteOn(110.0f, false, accent);
            int p = 0;
            for (int i = 0; i < SR / 10; ++i) {
                int s = std::abs(v.tick());
                if (s > p) p = s;
            }
            return p;
        };
        CHECK(peakFor(true) > peakFor(false));
    }

    {  // env decays after noteOff
        acid::dsp::TB303Stage2 v(SR);
        v.noteOn(110.0f, false, false);
        for (int i = 0; i < SR / 20; ++i) (void)v.tick();
        v.noteOff();
        long long first = 0, last = 0;
        int firstN = SR / 20;
        for (int i = 0; i < firstN; ++i) first += std::abs(v.tick());
        int total = SR * 3;
        for (int i = 0; i < total - 2 * firstN; ++i) (void)v.tick();
        for (int i = 0; i < firstN; ++i) last += std::abs(v.tick());
        CHECK(last * 4 < first);
    }

    {  // high resonance doesn't blow up i16 range
        acid::dsp::TB303Stage2 v(SR);
        v.setParams(acid::dsp::TB303Stage2Wave::Saw, 0.3f, 1.0f, 0.6f, 0.4f, 0.5f);
        v.noteOn(110.0f, false, false);
        int peak = 0;
        for (int i = 0; i < SR; ++i) {
            int s = std::abs(v.tick());
            if (s > peak) peak = s;
        }
        CHECK(peak < 32700);  // tolerate occasional clip but require no permanent pinning
    }

    {  // square path produces audio too
        acid::dsp::TB303Stage2 v(SR);
        v.setParams(acid::dsp::TB303Stage2Wave::Square, 0.4f, 0.7f, 0.6f, 0.4f, 0.5f);
        v.noteOn(110.0f, false, false);
        int peak = 0;
        for (int i = 0; i < SR / 10; ++i) {
            int s = std::abs(v.tick());
            if (s > peak) peak = s;
        }
        CHECK(peak > 500);
        CHECK(peak < 30000);
    }

    if (failures) {
        std::fprintf(stderr, "test_tb303s2_basic: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_tb303s2_basic: OK\n");
    return 0;
}
