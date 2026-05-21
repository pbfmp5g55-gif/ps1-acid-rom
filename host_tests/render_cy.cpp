// render_cy — fires a TR-808 CY voice, renders 4 s to cy.wav (long tail).
// Usage:  ./render_cy [decay=0.5] [brightness=0.5] [tune=0.5] [out=cy.wav]

#include "dsp/cy_six_squares.hpp"
#include "wav_writer.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    float decay      = argc > 1 ? std::atof(argv[1]) : 0.5f;
    float brightness = argc > 2 ? std::atof(argv[2]) : 0.5f;
    float tune       = argc > 3 ? std::atof(argv[3]) : 0.5f;
    std::string out  = argc > 4 ? argv[4] : "cy.wav";

    constexpr int SR = 22050;
    constexpr int DURATION_S = 4;

    acid::dsp::CYSixSquares cy(SR);
    cy.setParams(decay, brightness, tune);
    cy.trigger(1.0f);

    std::vector<int16_t> samples;
    samples.reserve(SR * DURATION_S);
    for (int i = 0; i < SR * DURATION_S; ++i) samples.push_back(cy.tick());

    if (!acid::host::write_wav_mono16(out, SR, samples)) {
        std::fprintf(stderr, "failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%d samples, %d Hz, decay=%.2f bright=%.2f tune=%.2f)\n",
                out.c_str(), static_cast<int>(samples.size()), SR, decay, brightness, tune);
    return 0;
}
