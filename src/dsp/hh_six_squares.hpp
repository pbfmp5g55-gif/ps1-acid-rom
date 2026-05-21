// hh_six_squares.hpp — TR-808-style hi-hat (closed/open share this topology).
//
// Reference: The TR-808 HH/CY generator runs six free-running square-wave
// oscillators at non-harmonic, irrational-ratio frequencies. Their sum is
// fed through a steep high-pass, then a band-pass with a peak around 8 kHz,
// then a VCA driven by an exponential decay envelope. Closed-HH and open-HH
// share the entire chain — only the envelope time constant differs (closed
// ≈ 50 ms, open ≈ 500 ms). The cymbal voice (CY) is the same again with a
// different envelope and a slightly different post-BPF.
//
// Discrete model:
//   - Six 32-bit phase accumulators incremented by per-oscillator step sizes.
//     Sign-bit of phase → square output. Non-harmonic ratios from a fixed
//     table (see .cpp) reproduce the metallic non-tonality.
//   - HPF: TPT SVF, "high" output at ~2 kHz cutoff.
//   - BPF: second TPT SVF, center ~6 kHz.
//   We use TPT (Zavalishin) here instead of Chamberlin because the
//   Chamberlin recurrence becomes unstable for the band of cutoffs we want
//   on HH/CY at SR=22050 (f + 1/Q > 2 region).
//   - Env: 1-pole exponential, time set by openness in [0, 1].

#pragma once
#include "fixed.hpp"
#include "svf_tpt.hpp"

namespace acid::dsp {

class HHSixSquares {
  public:
    explicit HHSixSquares(int sampleRate);

    // openness in [0, 1] — 0 = closed (~50 ms), 1 = open (~500 ms)
    // brightness in [0, 1] — shifts HPF/BPF cutoffs ±0.5 octave
    // tune in [0, 1] — scales the six oscillator frequencies ±0.5 octave
    void setParams(float openness, float brightness, float tune);

    void trigger(float velocity);
    i16 tick();
    bool isActive() const { return m_env > 32; }

  private:
    int m_sampleRate;

    // Six phase accumulators.
    u32 m_phase[6] = {0};
    u32 m_step[6] = {0};

    SVFTpt m_hp;
    SVFTpt m_bp;

    // Envelope.
    i32 m_env = 0, m_envCoeff = 0;
    i32 m_velocity = 0;
};

}  // namespace acid::dsp
