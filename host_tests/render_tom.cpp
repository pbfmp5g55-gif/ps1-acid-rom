// render_tom — fires a TR-808 Tom voice, renders 2 s to tom.wav.
// Usage:  ./render_tom [tuning=0.5] [tone=0.5] [decay=0.5] [out=tom.wav]

#include "dsp/tom_tbridge.hpp"
#include "wav_writer.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    float tuning = argc > 1 ? std::atof(argv[1]) : 0.5f;
    float tone   = argc > 2 ? std::atof(argv[2]) : 0.5f;
    float decay  = argc > 3 ? std::atof(argv[3]) : 0.5f;
    std::string out = argc > 4 ? argv[4] : "tom.wav";

    constexpr int SR = 22050;
    constexpr int DURATION_S = 2;

    acid::dsp::TomTBridge tom(SR);
    tom.setParams(tuning, tone, decay);
    tom.trigger(1.0f);

    std::vector<int16_t> samples;
    samples.reserve(SR * DURATION_S);
    for (int i = 0; i < SR * DURATION_S; ++i) samples.push_back(tom.tick());

    if (!acid::host::write_wav_mono16(out, SR, samples)) {
        std::fprintf(stderr, "failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%d samples, %d Hz, tuning=%.2f tone=%.2f decay=%.2f)\n",
                out.c_str(), static_cast<int>(samples.size()), SR, tuning, tone, decay);
    return 0;
}
