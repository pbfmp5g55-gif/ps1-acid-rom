// Sanity tests for CY voice.
// CY default decay=0.5 → tau ≈ 2.4 s. Window the tail check around a 6 s
// window to actually see significant decay.

#include "dsp/cy_six_squares.hpp"

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
        acid::dsp::CYSixSquares cy(SR);
        for (int i = 0; i < 200; ++i) CHECK(cy.tick() == 0);
        CHECK(!cy.isActive());
    }

    {
        acid::dsp::CYSixSquares cy(SR);
        cy.trigger(1.0f);
        bool nz = false;
        int peak = 0;
        for (int i = 0; i < SR / 10; ++i) {
            int s = cy.tick();
            if (s != 0) nz = true;
            if (std::abs(s) > peak) peak = std::abs(s);
        }
        CHECK(nz);
        CHECK(peak > 500);
        CHECK(peak < 30000);
    }

    {
        // CY sustains ~2.4 s by default. Use 8 s tail to see real decay; need
        // only 2× quieter (env still ~14% of start at t=5s, ~6% at 7s).
        acid::dsp::CYSixSquares cy(SR);
        cy.trigger(1.0f);
        long long first = 0, last = 0;
        int firstN = SR / 10;
        for (int i = 0; i < firstN; ++i) first += std::abs(cy.tick());
        int total = SR * 8;
        for (int i = 0; i < total - 2 * firstN; ++i) (void)cy.tick();
        for (int i = 0; i < firstN; ++i) last += std::abs(cy.tick());
        CHECK(last * 4 < first);
    }

    {
        auto peakFor = [](float v) {
            acid::dsp::CYSixSquares cy(SR);
            cy.trigger(v);
            int p = 0;
            for (int i = 0; i < SR / 10; ++i) {
                int s = std::abs(cy.tick());
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
        std::fprintf(stderr, "test_cy_basic: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_cy_basic: OK\n");
    return 0;
}
