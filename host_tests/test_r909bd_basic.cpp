// Sanity tests for 909 BD. Same invariants as BD plus a check that the click
// is audible in the very first few ms when attack > 0.

#include "dsp/r909_bd.hpp"

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

    {  // silent before trigger
        acid::dsp::R909BD bd(SR);
        for (int i = 0; i < 200; ++i) CHECK(bd.tick() == 0);
        CHECK(!bd.isActive());
    }

    {  // audible after trigger, with audible click in the first ~5 ms
        acid::dsp::R909BD bd(SR);
        bd.trigger(1.0f);
        int peakEarly = 0;
        int peakLater = 0;
        // Click window: first ~5 ms.
        for (int i = 0; i < SR / 200; ++i) {
            int s = std::abs(bd.tick());
            if (s > peakEarly) peakEarly = s;
        }
        // Body window: 10-100 ms.
        for (int i = 0; i < SR / 10 - SR / 200; ++i) {
            int s = std::abs(bd.tick());
            if (s > peakLater) peakLater = s;
        }
        CHECK(peakEarly > 1000);
        CHECK(peakLater > 1000);
        CHECK(peakEarly < 30000);
        CHECK(peakLater < 30000);
    }

    {  // attack=0 makes the click vanish (early peak drops vs attack=1)
        auto earlyPeak = [](float attack) {
            acid::dsp::R909BD bd(SR);
            bd.setParams(0.5f, 0.5f, 0.6f, attack);
            bd.trigger(1.0f);
            int p = 0;
            for (int i = 0; i < SR / 1000; ++i) {  // first 1 ms — body hasn't rung up yet
                int s = std::abs(bd.tick());
                if (s > p) p = s;
            }
            return p;
        };
        int pNo  = earlyPeak(0.0f);
        int pYes = earlyPeak(1.0f);
        CHECK(pYes > pNo);
    }

    {  // env decays
        acid::dsp::R909BD bd(SR);
        bd.trigger(1.0f);
        long long first = 0, last = 0;
        int firstN = SR / 10;
        for (int i = 0; i < firstN; ++i) first += std::abs(bd.tick());
        for (int i = 0; i < SR * 2 - 2 * firstN; ++i) (void)bd.tick();
        for (int i = 0; i < firstN; ++i) last += std::abs(bd.tick());
        CHECK(last * 4 < first);
    }

    if (failures) {
        std::fprintf(stderr, "test_r909bd_basic: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_r909bd_basic: OK\n");
    return 0;
}
