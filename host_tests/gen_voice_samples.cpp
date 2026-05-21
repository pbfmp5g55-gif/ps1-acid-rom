// gen_voice_samples — host-side build helper that pre-renders each voice
// once and emits a generated C++ header with the resulting PSX ADPCM data.
//
// Why this approach (M2 design note):
//   The PS1 build links with `-nostdlib -fno-builtin`, so cmath functions
//   (sin/exp/pow/tan used by the DSP voice setParams() helpers) are not
//   available. We render on the host (same bit-exact DSP code as the PS1
//   build), encode to PSX ADPCM, and embed the bytes in the binary.
//
// Usage:
//   ./gen_voice_samples > ../src/generated/voice_samples.h
//
// Re-run whenever any src/dsp/*.cpp coefficient changes or a new voice is
// added to the table below.

#include "adpcm_psx.hpp"
#include "dsp/bd_tbridge.hpp"
#include "dsp/sd_twint.hpp"
#include "dsp/tom_tbridge.hpp"
#include "dsp/hh_six_squares.hpp"
#include "dsp/cy_six_squares.hpp"
#include "dsp/cp_burst.hpp"
#include "dsp/cb_tbridge.hpp"
#include "dsp/tb303_stage1.hpp"
#include "dsp/r909_bd.hpp"
#include "dsp/r909_sd.hpp"
#include "dsp/tb303_stage2.hpp"

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr int SR = 22050;

template <class V, class Setup>
std::vector<int16_t> render_oneshot(int durationSamples, Setup setup) {
    V v(SR);
    setup(v);
    std::vector<int16_t> s;
    s.reserve(durationSamples);
    for (int i = 0; i < durationSamples; ++i) s.push_back(v.tick());
    return s;
}

void emit_array(const std::string &name, const std::vector<uint8_t> &data) {
    std::printf("alignas(4) inline constexpr uint8_t %s[%zu] = {\n",
                name.c_str(), data.size());
    for (size_t i = 0; i < data.size(); ++i) {
        if ((i % 16) == 0) std::printf("    ");
        std::printf("0x%02x", data[i]);
        if (i + 1 != data.size()) std::printf(",");
        if ((i % 16) == 15) std::printf("\n");
        else std::printf(" ");
    }
    if (data.size() % 16) std::printf("\n");
    std::printf("};\n");
    std::printf("inline constexpr unsigned %s_bytes = %zu;\n\n",
                name.c_str(), data.size());
}

void emit_voice(const std::string &name, const std::vector<int16_t> &samples) {
    auto adpcm = acid::host::adpcm::encode_one_shot(samples);
    emit_array(name + "_adpcm", adpcm);
    std::fprintf(stderr, "  %s: %d samples → %zu ADPCM bytes\n",
                 name.c_str(), static_cast<int>(samples.size()), adpcm.size());
}

}  // namespace

int main() {
    using namespace acid::dsp;

    // ---- 808 family (one-shot drum hits) ----

    auto bd = render_oneshot<BDTBridge>(SR * 3 / 2, [](auto &v) {
        v.setParams(0.5f, 0.6f, 0.5f);
        v.trigger(1.0f);
    });

    auto sd = render_oneshot<SDTwinT>(SR / 2, [](auto &v) {
        v.setParams(0.5f, 0.5f, 0.5f);
        v.trigger(1.0f);
    });

    auto tom = render_oneshot<TomTBridge>(SR, [](auto &v) {
        v.setParams(0.5f, 0.5f, 0.5f);
        v.trigger(1.0f);
    });

    auto hh = render_oneshot<HHSixSquares>(SR / 3, [](auto &v) {
        v.setParams(0.0f, 0.5f, 0.5f);  // closed
        v.trigger(1.0f);
    });

    // CY is intentionally long — keep the shimmer tail.
    auto cy = render_oneshot<CYSixSquares>(SR * 3, [](auto &v) {
        v.setParams(0.5f, 0.5f, 0.5f);
        v.trigger(1.0f);
    });

    auto cp = render_oneshot<CPBurst>(SR / 2, [](auto &v) {
        v.setParams(0.5f, 0.5f);
        v.trigger(1.0f);
    });

    auto cb = render_oneshot<CBTBridge>(SR / 5, [](auto &v) {
        v.setParams(0.5f, 0.5f, 0.5f);
        v.trigger(1.0f);
    });

    // ---- 303 (gated note: A1 root, release after 250 ms so the tail decays
    //      cleanly within the rendered window).
    auto render_303 = [](TB303Wave wave) {
        TB303Stage1 v(SR);
        v.setParams(wave, 0.4f, 0.7f, 0.6f, 0.4f, 0.5f);
        v.noteOn(55.0f, false, true);
        std::vector<int16_t> s;
        s.reserve(SR);
        for (int i = 0; i < SR; ++i) {
            if (i == SR / 4) v.noteOff();
            s.push_back(v.tick());
        }
        return s;
    };
    auto tb303_saw    = render_303(TB303Wave::Saw);
    auto tb303_square = render_303(TB303Wave::Square);

    // ---- 909 family.

    auto r909bd = render_oneshot<R909BD>(SR * 3 / 2, [](auto &v) {
        v.setParams(0.5f, 0.5f, 0.6f, 0.5f);
        v.trigger(1.0f);
    });

    auto r909sd = render_oneshot<R909SD>(SR / 2, [](auto &v) {
        v.setParams(0.5f, 0.5f, 0.5f);
        v.trigger(1.0f);
    });

    // ---- TB-303 stage 2 (Moog ladder) — 1 s one-shot at A1 with note-off
    //      release halfway, like stage 1's render.
    auto render_303s2 = [](TB303Stage2Wave wave) {
        TB303Stage2 v(SR);
        v.setParams(wave, 0.4f, 0.8f, 0.6f, 0.4f, 0.5f);
        v.noteOn(55.0f, false, true);
        std::vector<int16_t> s;
        s.reserve(SR);
        for (int i = 0; i < SR; ++i) {
            if (i == SR / 4) v.noteOff();
            s.push_back(v.tick());
        }
        return s;
    };
    auto tb303s2_saw    = render_303s2(TB303Stage2Wave::Saw);
    auto tb303s2_square = render_303s2(TB303Stage2Wave::Square);

    // ---- Emit.

    std::printf("// Auto-generated by host_tests/gen_voice_samples.cpp.\n");
    std::printf("// Do NOT edit by hand — re-run the generator instead.\n");
    std::printf("//\n");
    std::printf("// Source rate: %d Hz. Play with SPU sampleRate = 0x0800 (= 0.5x of 44.1).\n", SR);
    std::printf("\n#pragma once\n#include <cstdint>\n\n");
    std::printf("namespace acid::voice_samples {\n\n");

    std::fprintf(stderr, "rendering 13 voices @ %d Hz:\n", SR);
    emit_voice("bd",             bd);
    emit_voice("sd",             sd);
    emit_voice("tom",            tom);
    emit_voice("hh",             hh);
    emit_voice("cy",             cy);
    emit_voice("cp",             cp);
    emit_voice("cb",             cb);
    emit_voice("tb303_saw",      tb303_saw);
    emit_voice("tb303_square",   tb303_square);
    emit_voice("r909bd",         r909bd);
    emit_voice("r909sd",         r909sd);
    emit_voice("tb303s2_saw",    tb303s2_saw);
    emit_voice("tb303s2_square", tb303s2_square);

    std::printf("}  // namespace acid::voice_samples\n");
    return 0;
}
