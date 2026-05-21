// render_cb — fires a TR-808 claves voice, renders 0.5 s to cb.wav.
// Usage:  ./render_cb [tuning=0.5] [tone=0.5] [decay=0.5] [out=cb.wav]

#include "dsp/cb_tbridge.hpp"
#include "wav_writer.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    float tuning = argc > 1 ? std::atof(argv[1]) : 0.5f;
    float tone   = argc > 2 ? std::atof(argv[2]) : 0.5f;
    float decay  = argc > 3 ? std::atof(argv[3]) : 0.5f;
    std::string out = argc > 4 ? argv[4] : "cb.wav";

    constexpr int SR = 22050;
    constexpr int DURATION_SAMPLES = SR / 2;  // 0.5 s

    acid::dsp::CBTBridge cb(SR);
    cb.setParams(tuning, tone, decay);
    cb.trigger(1.0f);

    std::vector<int16_t> samples;
    samples.reserve(DURATION_SAMPLES);
    for (int i = 0; i < DURATION_SAMPLES; ++i) samples.push_back(cb.tick());

    if (!acid::host::write_wav_mono16(out, SR, samples)) {
        std::fprintf(stderr, "failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%d samples, %d Hz, tuning=%.2f tone=%.2f decay=%.2f)\n",
                out.c_str(), static_cast<int>(samples.size()), SR, tuning, tone, decay);
    return 0;
}
