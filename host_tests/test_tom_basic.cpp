// Sanity tests for Tom voice. Same template as test_bd_basic.cpp.

#include "dsp/tom_tbridge.hpp"

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
        acid::dsp::TomTBridge t(SR);
        for (int i = 0; i < 200; ++i) CHECK(t.tick() == 0);
        CHECK(!t.isActive());
    }

    {
        acid::dsp::TomTBridge t(SR);
        t.trigger(1.0f);
        bool nz = false;
        int peak = 0;
        for (int i = 0; i < SR / 10; ++i) {
            int s = t.tick();
            if (s != 0) nz = true;
            if (std::abs(s) > peak) peak = std::abs(s);
        }
        CHECK(nz);
        CHECK(peak > 1000);
        CHECK(peak < 30000);
    }

    {
        acid::dsp::TomTBridge t(SR);
        t.trigger(1.0f);
        long long first = 0, last = 0;
        int firstN = SR / 10;
        for (int i = 0; i < firstN; ++i) first += std::abs(t.tick());
        for (int i = 0; i < SR * 2 - 2 * firstN; ++i) (void)t.tick();
        for (int i = 0; i < firstN; ++i) last += std::abs(t.tick());
        CHECK(last * 4 < first);
    }

    {
        auto peakFor = [](float v) {
            acid::dsp::TomTBridge t(SR);
            t.trigger(v);
            int p = 0;
            for (int i = 0; i < SR / 10; ++i) {
                int s = std::abs(t.tick());
                if (s > p) p = s;
            }
            return p;
        };
        int pFull = peakFor(1.0f);
        int pHalf = peakFor(0.5f);
        CHECK(pHalf > 0);
        CHECK(pHalf * 3 > pFull);
        CHECK(pHalf * 2 < pFull + 1000);
    }

    if (failures) {
        std::fprintf(stderr, "test_tom_basic: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_tom_basic: OK\n");
    return 0;
}
