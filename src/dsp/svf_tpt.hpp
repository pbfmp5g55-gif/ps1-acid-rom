// svf_tpt.hpp — Zavalishin Topology-Preserving Transform state-variable
// filter. Stable for any cutoff up to Nyquist and any Q > 0, unlike the
// Chamberlin SVF (which we use elsewhere) whose stability breaks down for
// f + q_coeff > 2 — that hits us above ~5 kHz at SR=22050 with moderate Q.
//
// Used by HH / CY which need resonant filtering well into the upper octaves.
//
// Math (per Zavalishin "The Art of VA Filter Design", chapter on SVF):
//   g  = tan(pi * Fc / Fs)
//   k  = 1/Q              (damping)
//   a1 = 1 / (1 + g*(g + k))
//   a2 = g * a1
//   a3 = g * a2
//
// Per-sample, given input x and state {s1, s2}:
//   v3 = x - s2
//   v1 = a1*s1 + a2*v3
//   v2 = s2 + a2*s1 + a3*v3
//   s1 = 2*v1 - s1
//   s2 = 2*v2 - s2
// Outputs:
//   low  = v2
//   band = v1
//   high = v3 - k*v1 - v2
//
// We do this all in Q24 fixed-point with i64 intermediate accumulators.

#pragma once
#include "fixed.hpp"
#include <cmath>

namespace acid::dsp {

class SVFTpt {
  public:
    // For percussion voices we only ever want one tap, so wire it at the
    // call site. The tick() function returns {low, band, high} via output
    // parameters; cheap to ignore the ones you don't need.

    void setCutoff(int sampleRate, float fc, float Q) {
        if (Q < 0.05f) Q = 0.05f;
        // Limit fc to leave a bit of margin under Nyquist for tan() blowup.
        float maxF = 0.49f * static_cast<float>(sampleRate);
        if (fc > maxF) fc = maxF;
        if (fc < 1.0f) fc = 1.0f;
        float g = std::tan(3.14159265f * fc / static_cast<float>(sampleRate));
        float k = 1.0f / Q;
        float a1 = 1.0f / (1.0f + g * (g + k));
        float a2 = g * a1;
        float a3 = g * a2;
        m_a1 = to_q24(a1);
        m_a2 = to_q24(a2);
        m_a3 = to_q24(a3);
        m_k  = to_q24(k);
    }

    void reset() { m_s1 = m_s2 = 0; }

    // Returns band-pass output (Q24).
    i32 tickBand(i32 in) {
        i32 v3 = in - m_s2;
        i32 v1 = mul_q24(m_a1, m_s1) + mul_q24(m_a2, v3);
        i32 v2 = m_s2 + mul_q24(m_a2, m_s1) + mul_q24(m_a3, v3);
        m_s1 = (v1 << 1) - m_s1;
        m_s2 = (v2 << 1) - m_s2;
        return v1;
    }

    // Returns high-pass output (Q24).
    i32 tickHigh(i32 in) {
        i32 v3 = in - m_s2;
        i32 v1 = mul_q24(m_a1, m_s1) + mul_q24(m_a2, v3);
        i32 v2 = m_s2 + mul_q24(m_a2, m_s1) + mul_q24(m_a3, v3);
        m_s1 = (v1 << 1) - m_s1;
        m_s2 = (v2 << 1) - m_s2;
        return v3 - mul_q24(m_k, v1) - v2;
    }

  private:
    i32 m_a1 = 0, m_a2 = 0, m_a3 = 0, m_k = 0;
    i32 m_s1 = 0, m_s2 = 0;
};

}  // namespace acid::dsp
