// acb_tb303_stage1.hpp — Integer-only ACB-style TB-303 stage 1 voice for
// PS1 live render. Component-mapped layout:
//   DCO       saw / square, 32-bit phase accumulator
//   VCF       Chamberlin SVF lowpass (LOW tap), env-modulated cutoff
//   VCA       linear gain * VCA env, optional accent boost
//   Env (VCF) exponential decay from 1.0, drives cutoff modulation
//   Env (VCA) gate-held at 1.0, decays after noteOff (or with VCF env on
//             accent for a snappier feel)
//
// Differences from the host-only src/dsp/tb303_stage1.hpp:
//   - no float, no std::cmath; all math uses Q24 ints + qmath LUTs
//   - parameter setters take Q24 (caller does the float→Q24 conversion at
//     param-update time, NOT every sample)
//   - safe to compile and link under the PS1 toolchain
//
// Sample rate is locked to 44100 Hz to match the SPU streaming output.

#pragma once
#include "dsp/fixed.hpp"

namespace acid::voices {

using acid::dsp::i16;
using acid::dsp::i32;
using acid::dsp::u32;

enum class AcbWave : uint8_t { Saw = 0, Square = 1 };

class AcbTb303Stage1 {
  public:
    // Constants — sample-rate locked to the SPU stream so we can pre-bake
    // many of the per-sample coefficients into Q24 at param-set time.
    static constexpr int SAMPLE_RATE = 44100;

    AcbTb303Stage1();

    // ---- knobs (Q24 unit). 0..ONE represent 0..1. -----------------------
    void setWave(AcbWave w) { m_wave = w; }

    // cutoff: VCF base cutoff knob, Q24 in [0, 1] (mapped internally to a
    // log-ish Hz range 80..4000 Hz).
    void setCutoff(i32 cutoffQ24);
    // resonance: 0..1 → Q ≈ 1..12. Internally stored as Chamberlin 1/Q.
    void setResonance(i32 resQ24);
    // envMod: how strongly the VCF env opens cutoff. 0..1 → 0..3 octaves.
    void setEnvMod(i32 envModQ24);
    // decay: VCF+VCA env tau, 0..1 → 0.05..1.5 seconds.
    void setDecay(i32 decayQ24);
    // accentAmount: master accent depth, 0..1.
    void setAccentAmount(i32 accentQ24);

    // ---- gates ----------------------------------------------------------
    // Trigger a new note. `noteHz` is the pitch in integer Hz (we don't
    // need sub-Hz precision for a TB-303 — 1 Hz resolution at A2 is much
    // less than 1 cent). Holding Hz as i32 avoids 64-bit shifts in PS1.
    void noteOn(int noteHz, bool slide, bool accent);
    void noteOff();
    bool isActive() const { return m_envVca > 0x4000; }  // ~0.001

    // Render one PCM sample at SAMPLE_RATE. Result is in i16 audio domain.
    i16 tick();

  private:
    AcbWave m_wave = AcbWave::Saw;

    // DCO
    u32 m_phase     = 0;
    u32 m_step      = 0;
    u32 m_stepTarget= 0;
    u32 m_stepRate  = 0;
    bool m_sliding  = false;

    // VCF (Chamberlin SVF, Q24).
    i32 m_lpLow  = 0;
    i32 m_lpBand = 0;

    // Pre-baked knobs (all Q24).
    i32 m_cutoffParam = 0;     // 0..ONE
    i32 m_resonanceQc = 0;     // 1/Q
    i32 m_envMod      = 0;     // 0..ONE
    i32 m_envVcfCoeff = 0;     // exp decay coefficient per sample
    i32 m_envVcaCoeff = 0;
    i32 m_accentDepth = 0;     // 0..ONE

    // Envs.
    i32 m_envVcf       = 0;
    i32 m_envVca       = 0;
    i32 m_envVcaTarget = 0;

    bool m_accent = false;
};

}  // namespace acid::voices
