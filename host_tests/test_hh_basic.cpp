// Sanity tests for HH voice.
// HH default openness=0 → very short ~50 ms decay, so the decay-rate check
// uses a 0.5 s tail rather than 2 s.

#include "dsp/hh_six_squares.hpp"

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
        // Silent before trigger: env starts at 0 so output should be 0 even
        // though the six squares run free.
        acid::dsp::HHSixSquares hh(SR);
        for (int i = 0; i < 200; ++i) CHECK(hh.tick() == 0);
        CHECK(!hh.isActive());
    }

    {
        acid::dsp::HHSixSquares hh(SR);
        hh.trigger(1.0f);
        bool nz = false;
        int peak = 0;
        // First ~30 ms covers the env head before it decays substantially.
        for (int i = 0; i < SR / 30; ++i) {
            int s = hh.tick();
            if (s != 0) nz = true;
            if (std::abs(s) > peak) peak = std::abs(s);
        }
        CHECK(nz);
        CHECK(peak > 500);  // HH is brighter / quieter peak than BD
        CHECK(peak < 30000);
    }

    {
        // Decay: in default (closed) mode tau ≈ 50 ms, so 500 ms tail should
        // be near silence.
        acid::dsp::HHSixSquares hh(SR);
        hh.trigger(1.0f);
        long long first = 0, last = 0;
        int firstN = SR / 30;  // ~33 ms windows
        for (int i = 0; i < firstN; ++i) first += std::abs(hh.tick());
        int total = SR / 2;  // 500 ms
        for (int i = 0; i < total - 2 * firstN; ++i) (void)hh.tick();
        for (int i = 0; i < firstN; ++i) last += std::abs(hh.tick());
        CHECK(last * 10 < first);  // very strong decay expected
    }

    {
        auto peakFor = [](float v) {
            acid::dsp::HHSixSquares hh(SR);
            hh.trigger(v);
            int p = 0;
            for (int i = 0; i < SR / 30; ++i) {
                int s = std::abs(hh.tick());
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
        std::fprintf(stderr, "test_hh_basic: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_hh_basic: OK\n");
    return 0;
}
