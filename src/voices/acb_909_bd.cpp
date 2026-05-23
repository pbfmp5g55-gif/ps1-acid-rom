// acb_909_bd.cpp — see acb_909_bd.hpp.

#include "voices/acb_909_bd.hpp"
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

constexpr int CLICK_TAU_SAMPLES = 220;  // ~5 ms at 44100 Hz

}  // namespace

Acb909Bd::Acb909Bd() {
    m_clickEnvCoeff = decay_coeff(CLICK_TAU_SAMPLES);
    setTuning(static_cast<i32>(0.50 * ONE_Q24));
    setTone  (static_cast<i32>(0.50 * ONE_Q24));
    setDecay (static_cast<i32>(0.50 * ONE_Q24));
    setAttack(static_cast<i32>(0.50 * ONE_Q24));
}

void Acb909Bd::setTuning(i32 tuningQ24) {
    if (tuningQ24 < 0) tuningQ24 = 0;
    if (tuningQ24 > ONE_Q24) tuningQ24 = ONE_Q24;
    int hz = 55 + ((55 * tuningQ24) >> Q24_SHIFT);  // 55..110
    m_f = svf_f_from_hz(hz);
}

void Acb909Bd::setTone(i32 toneQ24) {
    if (toneQ24 < 0) toneQ24 = 0;
    if (toneQ24 > ONE_Q24) toneQ24 = ONE_Q24;
    constexpr i32 INV_Q_LO = ONE_Q24 / 4;    // Q=4
    constexpr i32 INV_Q_HI = ONE_Q24 / 30;   // Q=30
    m_q = lerp_q24(INV_Q_LO, INV_Q_HI, toneQ24);
}

void Acb909Bd::setDecay(i32 decayQ24) {
    if (decayQ24 < 0) decayQ24 = 0;
    if (decayQ24 > ONE_Q24) decayQ24 = ONE_Q24;
    constexpr int TAU_LO = 3528;    // 80 ms
    constexpr int TAU_HI = 52920;   // 1.2 s
    int tau = TAU_LO +
        static_cast<int>(
            (static_cast<int64_t>(TAU_HI - TAU_LO) * decayQ24) >> Q24_SHIFT);
    m_envCoeff = decay_coeff(tau);
}

void Acb909Bd::setAttack(i32 attackQ24) {
    if (attackQ24 < 0) attackQ24 = 0;
    if (attackQ24 > ONE_Q24) attackQ24 = ONE_Q24;
    // Click gain caps at 0.6 mix-level.
    m_clickGain = mul_q24(attackQ24, (6 * ONE_Q24) / 10);
}

void Acb909Bd::trigger(i32 velocityQ24) {
    if (velocityQ24 < 0) velocityQ24 = 0;
    if (velocityQ24 > ONE_Q24) velocityQ24 = ONE_Q24;
    m_pendingImpulse = velocityQ24 * 14;  // 909 hits harder than 808
    m_env = ONE_Q24;
    m_clickEnv = velocityQ24;
}

acid::dsp::i16 Acb909Bd::tick() {
    i32 in = m_pendingImpulse;
    m_pendingImpulse = 0;

    m_low += mul_q24(m_f, m_band);
    i32 high = in - m_low - mul_q24(m_q, m_band);
    m_band += mul_q24(m_f, high);

    i32 body = mul_q24(m_band, m_env);
    m_env = mul_q24(m_env, m_envCoeff);

    i32 click = mul_q24(m_noise.tick(), m_clickGain);
    click = mul_q24(click, m_clickEnv);
    m_clickEnv = mul_q24(m_clickEnv, m_clickEnvCoeff);

    i32 out = body + click;

    i32 scaled = out >> (Q24_SHIFT - 14);
    return sat16(scaled);
}

}  // namespace acid::voices
