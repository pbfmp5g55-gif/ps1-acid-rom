// cy_six_squares.hpp — TR-808-style cymbal (CY).
//
// Shares the front-end with HH (six non-harmonic squares → HPF → BPF) but:
//   - BPF tuned a little lower (~6 kHz vs HH's ~6 kHz at default brightness;
//     CY's BPF defaults can stay similar — what really differentiates is the
//     much longer envelope and the Q)
//   - Envelope much longer (~0.8 s .. ~4 s)
//   - Higher Q on the BPF → ringier shimmer tail
//
// Uses the same TPT SVF helper as HH (stable at high freqs / high Q).

#pragma once
#include "fixed.hpp"
#include "svf_tpt.hpp"

namespace acid::dsp {

class CYSixSquares {
  public:
    explicit CYSixSquares(int sampleRate);

    // decay      in [0, 1] — env tau (~0.8 s .. ~4 s)
    // brightness in [0, 1] — shifts HPF/BPF cutoffs ±0.5 octave
    // tune       in [0, 1] — scales the six oscillator frequencies ±0.5 oct
    void setParams(float decay, float brightness, float tune);

    void trigger(float velocity);
    i16 tick();
    bool isActive() const { return m_env > 32; }

  private:
    int m_sampleRate;

    u32 m_phase[6] = {0};
    u32 m_step[6] = {0};

    SVFTpt m_hp;
    SVFTpt m_bp;

    i32 m_env = 0, m_envCoeff = 0;
    i32 m_velocity = 0;
};

}  // namespace acid::dsp
