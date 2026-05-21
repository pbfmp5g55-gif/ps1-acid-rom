// render_hh — fires a TR-808 HH voice, renders 2 s to hh.wav.
// Usage:  ./render_hh [openness=0.0] [brightness=0.5] [tune=0.5] [out=hh.wav]
// openness 0.0 = closed (short), 1.0 = open (long).

#include "dsp/hh_six_squares.hpp"
#include "wav_writer.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    float openness   = argc > 1 ? std::atof(argv[1]) : 0.0f;
    float brightness = argc > 2 ? std::atof(argv[2]) : 0.5f;
    float tune       = argc > 3 ? std::atof(argv[3]) : 0.5f;
    std::string out  = argc > 4 ? argv[4] : "hh.wav";

    constexpr int SR = 22050;
    constexpr int DURATION_S = 2;

    acid::dsp::HHSixSquares hh(SR);
    hh.setParams(openness, brightness, tune);
    hh.trigger(1.0f);

    std::vector<int16_t> samples;
    samples.reserve(SR * DURATION_S);
    for (int i = 0; i < SR * DURATION_S; ++i) samples.push_back(hh.tick());

    if (!acid::host::write_wav_mono16(out, SR, samples)) {
        std::fprintf(stderr, "failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%d samples, %d Hz, open=%.2f bright=%.2f tune=%.2f)\n",
                out.c_str(), static_cast<int>(samples.size()), SR, openness, brightness, tune);
    return 0;
}
