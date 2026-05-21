// Sanity tests for CB (claves) voice.
// Decay is very short (~50-100 ms) so test windows are tighter.

#include "dsp/cb_tbridge.hpp"

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
        acid::dsp::CBTBridge cb(SR);
        for (int i = 0; i < 200; ++i) CHECK(cb.tick() == 0);
        CHECK(!cb.isActive());
    }

    {
        acid::dsp::CBTBridge cb(SR);
        cb.trigger(1.0f);
        bool nz = false;
        int peak = 0;
        for (int i = 0; i < SR / 30; ++i) {  // ~30 ms — the whole hit
            int s = cb.tick();
            if (s != 0) nz = true;
            if (std::abs(s) > peak) peak = std::abs(s);
        }
        CHECK(nz);
        CHECK(peak > 500);
        CHECK(peak < 30000);
    }

    {
        // Default decay tau ≈ 70 ms. After 300 ms we should be ~deep in tail.
        acid::dsp::CBTBridge cb(SR);
        cb.trigger(1.0f);
        long long first = 0, last = 0;
        int firstN = SR / 30;
        for (int i = 0; i < firstN; ++i) first += std::abs(cb.tick());
        int total = SR / 3;  // ~333 ms
        for (int i = 0; i < total - 2 * firstN; ++i) (void)cb.tick();
        for (int i = 0; i < firstN; ++i) last += std::abs(cb.tick());
        CHECK(last * 8 < first);
    }

    {
        auto peakFor = [](float v) {
            acid::dsp::CBTBridge cb(SR);
            cb.trigger(v);
            int p = 0;
            for (int i = 0; i < SR / 30; ++i) {
                int s = std::abs(cb.tick());
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
        std::fprintf(stderr, "test_cb_basic: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_cb_basic: OK\n");
    return 0;
}
