// acb_808_cb.cpp — see acb_808_cb.hpp.

#include "voices/acb_808_cb.hpp"
#include "dsp/qmath.hpp"

namespace acid::voices {

using acid::dsp::mul_q24;
using acid::dsp::sat16;
using acid::dsp::Q24_SHIFT;
constexpr acid::dsp::i32 ONE_Q24 = 1 << Q24_SHIFT;

namespace {

constexpr acid::dsp::i32 TWO_PI_OVER_SR_Q24 = 2390;

inline acid::dsp::i32 svf_f_from_hz(int hz) { return hz * TWO_PI_OVER_SR_Q24; }

inline acid::dsp::i32 lerp_q24(acid::dsp::i32 a, acid::dsp::i32 b,
                               acid::dsp::i32 t) {
    return a + mul_q24(b - a, t);
}

inline acid::dsp::i32 decay_coeff(int tau_samples) {
    if (tau_samples < 1) tau_samples = 1;
    return acid::dsp::exp_q24(-(ONE_Q24 / tau_samples));
}

}  // namespace

Acb808Cb::Acb808Cb() {
    setTuning(static_cast<i32>(0.50 * ONE_Q24));
    setTone  (static_cast<i32>(0.50 * ONE_Q24));
    setDecay (static_cast<i32>(0.50 * ONE_Q24));
}

void Acb808Cb::setTuning(i32 tuningQ24) {
    if (tuningQ24 < 0) tuningQ24 = 0;
    if (tuningQ24 > ONE_Q24) tuningQ24 = ONE_Q24;
    int hz = 1500 + ((2000 * tuningQ24) >> Q24_SHIFT);  // 1500..3500
    m_f = svf_f_from_hz(hz);
    // At 3500 Hz the Chamberlin f coefficient is ~0.5, still well within
    // stability (f + 1/Q < 2 for Q≥6 → 1/Q ≤ 1/6 → max 0.67).
}

void Acb808Cb::setTone(i32 toneQ24) {
    if (toneQ24 < 0) toneQ24 = 0;
    if (toneQ24 > ONE_Q24) toneQ24 = ONE_Q24;
    constexpr i32 INV_Q_LO = ONE_Q24 / 6;    // Q=6
    constexpr i32 INV_Q_HI = ONE_Q24 / 20;   // Q=20
    m_q = lerp_q24(INV_Q_LO, INV_Q_HI, toneQ24);
}

void Acb808Cb::setDecay(i32 decayQ24) {
    if (decayQ24 < 0) decayQ24 = 0;
    if (decayQ24 > ONE_Q24) decayQ24 = ONE_Q24;
    constexpr int TAU_LO = 882;    // 20 ms
    constexpr int TAU_HI = 5292;   // 120 ms
    int tau = TAU_LO +
        static_cast<int>(
            (static_cast<int64_t>(TAU_HI - TAU_LO) * decayQ24) >> Q24_SHIFT);
    m_envCoeff = decay_coeff(tau);
}

void Acb808Cb::trigger(i32 velocityQ24) {
    if (velocityQ24 < 0) velocityQ24 = 0;
    if (velocityQ24 > ONE_Q24) velocityQ24 = ONE_Q24;
    // High f0 → small impulse is enough.
    m_pendingImpulse = (velocityQ24 * 3) / 2;  // ~1.5
    m_env = ONE_Q24;
}

acid::dsp::i16 Acb808Cb::tick() {
    i32 in = m_pendingImpulse;
    m_pendingImpulse = 0;

    m_low += mul_q24(m_f, m_band);
    i32 high = in - m_low - mul_q24(m_q, m_band);
    m_band += mul_q24(m_f, high);

    i32 out = mul_q24(m_band, m_env);
    m_env = mul_q24(m_env, m_envCoeff);

    i32 scaled = out >> (Q24_SHIFT - 14);
    return sat16(scaled);
}

}  // namespace acid::voices
