// render_cp — fires a TR-808 cowbell voice, renders 1 s to cp.wav.
// Usage:  ./render_cp [tuning=0.5] [decay=0.5] [out=cp.wav]

#include "dsp/cp_burst.hpp"
#include "wav_writer.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    float tuning = argc > 1 ? std::atof(argv[1]) : 0.5f;
    float decay  = argc > 2 ? std::atof(argv[2]) : 0.5f;
    std::string out = argc > 3 ? argv[3] : "cp.wav";

    constexpr int SR = 22050;
    constexpr int DURATION_S = 1;

    acid::dsp::CPBurst cp(SR);
    cp.setParams(tuning, decay);
    cp.trigger(1.0f);

    std::vector<int16_t> samples;
    samples.reserve(SR * DURATION_S);
    for (int i = 0; i < SR * DURATION_S; ++i) samples.push_back(cp.tick());

    if (!acid::host::write_wav_mono16(out, SR, samples)) {
        std::fprintf(stderr, "failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%d samples, %d Hz, tuning=%.2f decay=%.2f)\n",
                out.c_str(), static_cast<int>(samples.size()), SR, tuning, decay);
    return 0;
}
