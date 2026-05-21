// render_sd — fires a TR-808 SD voice, renders 2 s to sd.wav.
// Usage:  ./render_sd [tuning=0.5] [snappy=0.5] [decay=0.5] [out=sd.wav]

#include "dsp/sd_twint.hpp"
#include "wav_writer.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    float tuning = argc > 1 ? std::atof(argv[1]) : 0.5f;
    float snappy = argc > 2 ? std::atof(argv[2]) : 0.5f;
    float decay  = argc > 3 ? std::atof(argv[3]) : 0.5f;
    std::string out = argc > 4 ? argv[4] : "sd.wav";

    constexpr int SR = 22050;
    constexpr int DURATION_S = 2;

    acid::dsp::SDTwinT sd(SR);
    sd.setParams(tuning, snappy, decay);
    sd.trigger(1.0f);

    std::vector<int16_t> samples;
    samples.reserve(SR * DURATION_S);
    for (int i = 0; i < SR * DURATION_S; ++i) samples.push_back(sd.tick());

    if (!acid::host::write_wav_mono16(out, SR, samples)) {
        std::fprintf(stderr, "failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%d samples, %d Hz, tuning=%.2f snappy=%.2f decay=%.2f)\n",
                out.c_str(), static_cast<int>(samples.size()), SR, tuning, snappy, decay);
    return 0;
}
