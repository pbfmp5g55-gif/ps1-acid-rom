// render_r909bd — fires a TR-909 BD voice, renders 2 s to r909bd.wav.

#include "dsp/r909_bd.hpp"
#include "wav_writer.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    float tuning = argc > 1 ? std::atof(argv[1]) : 0.5f;
    float tone   = argc > 2 ? std::atof(argv[2]) : 0.5f;
    float decay  = argc > 3 ? std::atof(argv[3]) : 0.6f;
    float attack = argc > 4 ? std::atof(argv[4]) : 0.5f;
    std::string out = argc > 5 ? argv[5] : "r909bd.wav";

    constexpr int SR = 22050;
    constexpr int DURATION_S = 2;

    acid::dsp::R909BD bd(SR);
    bd.setParams(tuning, tone, decay, attack);
    bd.trigger(1.0f);

    std::vector<int16_t> samples;
    samples.reserve(SR * DURATION_S);
    for (int i = 0; i < SR * DURATION_S; ++i) samples.push_back(bd.tick());

    if (!acid::host::write_wav_mono16(out, SR, samples)) {
        std::fprintf(stderr, "failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%d samples, %d Hz, tune=%.2f tone=%.2f decay=%.2f attack=%.2f)\n",
                out.c_str(), static_cast<int>(samples.size()), SR, tuning, tone, decay, attack);
    return 0;
}
