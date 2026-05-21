// Sanity tests for SD voice. See test_bd_basic.cpp for what we check and why.

#include "dsp/sd_twint.hpp"

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
        acid::dsp::SDTwinT sd(SR);
        for (int i = 0; i < 200; ++i) CHECK(sd.tick() == 0);
        CHECK(!sd.isActive());
    }

    {  // non-zero peak after trigger
        acid::dsp::SDTwinT sd(SR);
        sd.trigger(1.0f);
        bool nz = false;
        int peak = 0;
        for (int i = 0; i < SR / 10; ++i) {
            int s = sd.tick();
            if (s != 0) nz = true;
            if (std::abs(s) > peak) peak = std::abs(s);
        }
        CHECK(nz);
        CHECK(peak > 1000);
        CHECK(peak < 30000);
    }

    {  // env decays — tail (last 100 ms of 1.5 s window) much quieter than first 100 ms
        acid::dsp::SDTwinT sd(SR);
        sd.trigger(1.0f);
        long long first = 0, last = 0;
        int firstN = SR / 10;
        for (int i = 0; i < firstN; ++i) first += std::abs(sd.tick());
        int total = static_cast<int>(SR * 1.5);
        for (int i = 0; i < total - 2 * firstN; ++i) (void)sd.tick();
        for (int i = 0; i < firstN; ++i) last += std::abs(sd.tick());
        CHECK(last * 4 < first);
    }

    {  // velocity scaling
        auto peakFor = [](float v) {
            acid::dsp::SDTwinT sd(SR);
            sd.trigger(v);
            int p = 0;
            for (int i = 0; i < SR / 10; ++i) {
                int s = std::abs(sd.tick());
                if (s > p) p = s;
            }
            return p;
        };
        int pFull = peakFor(1.0f);
        int pHalf = peakFor(0.5f);
        CHECK(pHalf > 0);
        CHECK(pHalf * 3 > pFull);
        CHECK(pHalf * 2 < pFull + 1500);  // slightly more slack — SD has two paths
    }

    if (failures) {
        std::fprintf(stderr, "test_sd_basic: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_sd_basic: OK\n");
    return 0;
}
