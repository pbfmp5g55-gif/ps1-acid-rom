// render_tb303s2 — TB-303 stage 2 (Moog ladder), 8-note acid lick, 4 s wav.

#include "dsp/tb303_stage2.hpp"
#include "wav_writer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    acid::dsp::TB303Stage2Wave w = acid::dsp::TB303Stage2Wave::Saw;
    int argi = 1;
    if (argc > argi && std::strcmp(argv[argi], "square") == 0) {
        w = acid::dsp::TB303Stage2Wave::Square; ++argi;
    } else if (argc > argi && std::strcmp(argv[argi], "saw") == 0) {
        ++argi;
    }
    float cutoff = argc > argi ? std::atof(argv[argi++]) : 0.4f;
    float reso   = argc > argi ? std::atof(argv[argi++]) : 0.8f;  // higher than stage1 default — show off resonance
    float envMod = argc > argi ? std::atof(argv[argi++]) : 0.6f;
    float decay  = argc > argi ? std::atof(argv[argi++]) : 0.4f;
    float accent = argc > argi ? std::atof(argv[argi++]) : 0.5f;
    std::string out = argc > argi ? argv[argi++] : "tb303s2.wav";

    constexpr int SR = 22050;
    constexpr int DURATION_S = 4;

    acid::dsp::TB303Stage2 v(SR);
    v.setParams(w, cutoff, reso, envMod, decay, accent);

    struct Note { float hz; bool slide; bool accent; };
    Note pattern[8] = {
        {55.0f,  false, true },
        {110.0f, false, false},
        {55.0f,  true,  false},
        {82.4f,  false, true },
        {55.0f,  false, false},
        {65.4f,  false, false},
        {73.4f,  true,  true },
        {55.0f,  false, false},
    };

    const int samplesPerNote = SR * 60 / 140 / 4;
    std::vector<int16_t> samples;
    samples.reserve(SR * DURATION_S);
    int noteIdx = 0, sampleInNote = 0;
    v.noteOn(pattern[0].hz, false, pattern[0].accent);
    for (int i = 0; i < SR * DURATION_S; ++i) {
        if (sampleInNote == samplesPerNote) {
            sampleInNote = 0;
            noteIdx = (noteIdx + 1) % 8;
            v.noteOn(pattern[noteIdx].hz, pattern[noteIdx].slide,
                     pattern[noteIdx].accent);
        }
        samples.push_back(v.tick());
        ++sampleInNote;
    }

    if (!acid::host::write_wav_mono16(out, SR, samples)) {
        std::fprintf(stderr, "failed to write %s\n", out.c_str());
        return 1;
    }
    std::printf("wrote %s (%d samples, %d Hz, wave=%s cutoff=%.2f reso=%.2f envMod=%.2f decay=%.2f accent=%.2f)\n",
                out.c_str(), static_cast<int>(samples.size()), SR,
                w == acid::dsp::TB303Stage2Wave::Saw ? "saw" : "square",
                cutoff, reso, envMod, decay, accent);
    return 0;
}
