// Sanity tests for BD voice. Not perceptual tests (those are by-ear with the
// rendered wav), but invariants we don't want to silently break in refactors.

#include "dsp/bd_tbridge.hpp"

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

    // 1. After construction with no trigger, output is silent.
    {
        acid::dsp::BDTBridge bd(SR);
        for (int i = 0; i < 200; ++i) CHECK(bd.tick() == 0);
        CHECK(!bd.isActive());
    }

    // 2. After trigger, output is nonzero within the first few ms.
    {
        acid::dsp::BDTBridge bd(SR);
        bd.trigger(1.0f);
        bool sawNonZero = false;
        int peakAbs = 0;
        for (int i = 0; i < SR / 10; ++i) {  // 100 ms
            int s = bd.tick();
            if (s != 0) sawNonZero = true;
            if (std::abs(s) > peakAbs) peakAbs = std::abs(s);
        }
        CHECK(sawNonZero);
        // Peak should be loud but not pinned to i16 max (means we'd be clipping
        // hard already with a single voice).
        CHECK(peakAbs > 1000);
        CHECK(peakAbs < 30000);
    }

    // 3. Envelope decays — energy in last 100 ms < first 100 ms.
    {
        acid::dsp::BDTBridge bd(SR);
        bd.trigger(1.0f);
        long long sumFirst = 0, sumLast = 0;
        int firstN = SR / 10;
        for (int i = 0; i < firstN; ++i) sumFirst += std::abs(bd.tick());
        // skip to near the end of a 2 second tail
        for (int i = 0; i < SR * 2 - 2 * firstN; ++i) (void)bd.tick();
        for (int i = 0; i < firstN; ++i) sumLast += std::abs(bd.tick());
        CHECK(sumLast * 4 < sumFirst);  // at least 4× quieter
    }

    // 4. Velocity scales peak roughly proportionally.
    {
        auto peakFor = [](float v) {
            acid::dsp::BDTBridge bd(SR);
            bd.trigger(v);
            int p = 0;
            for (int i = 0; i < SR / 10; ++i) {
                int s = std::abs(bd.tick());
                if (s > p) p = s;
            }
            return p;
        };
        int pFull = peakFor(1.0f);
        int pHalf = peakFor(0.5f);
        CHECK(pHalf > 0);
        CHECK(pHalf * 3 > pFull);  // 0.5v ≥ ~33% of 1.0v
        CHECK(pHalf * 2 < pFull + 1000);  // and ≤ ~50% (give some slack)
    }

    if (failures) {
        std::fprintf(stderr, "test_bd_basic: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_bd_basic: OK\n");
    return 0;
}
