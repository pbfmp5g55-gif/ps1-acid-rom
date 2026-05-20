// render_bd — fires a TR-808 BD voice and renders 2 seconds to bd.wav.
//
// Usage:  ./render_bd [tone=0.5] [decay=0.6] [tuning=0.5] [out=bd.wav]
//
// Listen to the output wav to judge whether the topology + coefficients in
// bd_tbridge.cpp sound like a real TR-808 BD. Iterate the .cpp; rerun.

#include "dsp/bd_tbridge.hpp"
#include "wav_writer.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    float tone   = argc > 1 ? std::atof(argv[1]) : 0.5f;
    float decay  = argc > 2 ? std::atof(argv[2]) : 0.6f;
    float tuning = argc > 3 ? std::atof(argv[3]) : 0.5f;
    std::string out = argc > 4 ? argv[4] : "bd.wav";

    constexpr int SR = 22050;
    constexpr int DURATION_S = 2;

    acid::dsp::BDTBridge bd(SR);
    bd.setParams(tone, decay, tuning);
    bd.trigger(1.0f);

    std::vector<int16_t> samples;
    samples.reserve(SR * DURATION_S);
    for (int i = 0; i < SR * DURATION_S; ++i) {
        samples.push_back(bd.tick());
    }

    if (!acid::host::write_wav_mono16(out, SR, samples)) {
        std::fprintf(stderr, "failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%d samples, %d Hz, tone=%.2f decay=%.2f tuning=%.2f)\n",
                out.c_str(), static_cast<int>(samples.size()), SR, tone, decay, tuning);
    return 0;
}
