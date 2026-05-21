// render_tb303 — fires a TB-303 stage-1 voice playing a tiny acid pattern,
// renders ~4 s to tb303.wav.
//
// Usage:
//   ./render_tb303 [saw|square] [cutoff=0.4] [reso=0.7] [envMod=0.6] [decay=0.4] [accent=0.5] [out=tb303.wav]
//
// Pattern is a fixed 8-note acid lick (A1 root, slides + accents) so the
// listener can immediately judge whether the env-mod sweep and the
// accent/slide behaviour sound right.

#include "dsp/tb303_stage1.hpp"
#include "wav_writer.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char **argv) {
    acid::dsp::TB303Wave w = acid::dsp::TB303Wave::Saw;
    int argi = 1;
    if (argc > argi && std::strcmp(argv[argi], "square") == 0) {
        w = acid::dsp::TB303Wave::Square;
        ++argi;
    } else if (argc > argi && std::strcmp(argv[argi], "saw") == 0) {
        ++argi;
    }
    float cutoff = argc > argi ? std::atof(argv[argi++]) : 0.4f;
    float reso   = argc > argi ? std::atof(argv[argi++]) : 0.7f;
    float envMod = argc > argi ? std::atof(argv[argi++]) : 0.6f;
    float decay  = argc > argi ? std::atof(argv[argi++]) : 0.4f;
    float accent = argc > argi ? std::atof(argv[argi++]) : 0.5f;
    std::string out = argc > argi ? argv[argi++] : "tb303.wav";

    constexpr int SR = 22050;
    constexpr int DURATION_S = 4;

    acid::dsp::TB303Stage1 v(SR);
    v.setParams(w, cutoff, reso, envMod, decay, accent);

    // Tiny 8-note acid pattern at 140 BPM (16ths). Frequencies in A minor-ish.
    struct Note { float hz; bool slide; bool accent; };
    Note pattern[8] = {
        {55.0f,  false, true },  // A1, accent
        {110.0f, false, false},  // A2
        {55.0f,  true,  false},  // A1 slide
        {82.4f,  false, true },  // E2 accent
        {55.0f,  false, false},  // A1
        {65.4f,  false, false},  // C2
        {73.4f,  true,  true },  // D2 slide accent
        {55.0f,  false, false},  // A1
    };

    const int samplesPerNote = SR * 60 / 140 / 4;  // 16th @ 140 BPM ≈ 2360 samples

    std::vector<int16_t> samples;
    samples.reserve(SR * DURATION_S);
    int noteIdx = 0;
    int sampleInNote = 0;
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
                w == acid::dsp::TB303Wave::Saw ? "saw" : "square",
                cutoff, reso, envMod, decay, accent);
    return 0;
}
