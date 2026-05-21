// Sanity tests for TB303 stage 1.

#include "dsp/tb303_stage1.hpp"

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

    {  // silent before noteOn (env starts at 0)
        acid::dsp::TB303Stage1 v(SR);
        for (int i = 0; i < 200; ++i) CHECK(v.tick() == 0);
        CHECK(!v.isActive());
    }

    {  // after noteOn we get audible output
        acid::dsp::TB303Stage1 v(SR);
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

    {  // accent makes the note louder than a non-accent note at same pitch
        auto peakFor = [](bool accent) {
            acid::dsp::TB303Stage1 v(SR);
            v.noteOn(110.0f, false, accent);
            int p = 0;
            for (int i = 0; i < SR / 10; ++i) {
                int s = std::abs(v.tick());
                if (s > p) p = s;
            }
            return p;
        };
        int p0 = peakFor(false);
        int p1 = peakFor(true);
        CHECK(p1 > p0);  // accent boosts something (env mod + vca)
    }

    {  // env decays — after noteOff the voice goes quiet over time
        acid::dsp::TB303Stage1 v(SR);
        v.noteOn(110.0f, false, false);
        // Let it ring briefly then release.
        for (int i = 0; i < SR / 20; ++i) (void)v.tick();
        v.noteOff();
        long long first = 0, last = 0;
        int firstN = SR / 20;
        for (int i = 0; i < firstN; ++i) first += std::abs(v.tick());
        // Skip to near the end of a 3 s tail.
        int total = SR * 3;
        for (int i = 0; i < total - 2 * firstN; ++i) (void)v.tick();
        for (int i = 0; i < firstN; ++i) last += std::abs(v.tick());
        CHECK(last * 4 < first);
    }

    {  // slide ramps the pitch — after noteOn with slide=true, voice ends up
        // active and audible at the new pitch (just check it didn't go silent
        // or explode).
        acid::dsp::TB303Stage1 v(SR);
        v.noteOn(110.0f, false, false);
        for (int i = 0; i < SR / 50; ++i) (void)v.tick();
        v.noteOn(220.0f, true, false);
        int peak = 0;
        for (int i = 0; i < SR / 10; ++i) {
            int s = std::abs(v.tick());
            if (s > peak) peak = s;
        }
        CHECK(peak > 500);
        CHECK(peak < 30000);
    }

    {  // square wave path also produces audio
        acid::dsp::TB303Stage1 v(SR);
        v.setParams(acid::dsp::TB303Wave::Square, 0.4f, 0.7f, 0.6f, 0.4f, 0.5f);
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
        std::fprintf(stderr, "test_tb303_basic: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_tb303_basic: OK\n");
    return 0;
}
