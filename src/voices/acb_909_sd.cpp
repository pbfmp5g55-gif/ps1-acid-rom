// acb_909_sd.cpp — see acb_909_sd.hpp.

#include "voices/acb_909_sd.hpp"
#include "dsp/qmath.hpp"

namespace acid::voices {

using acid::dsp::mul_q24;
using acid::dsp::sat16;
using acid::dsp::Q24_SHIFT;
constexpr acid::dsp::i32 ONE_Q24 = 1 << Q24_SHIFT;

namespace {

constexpr acid::dsp::i32 TWO_PI_OVER_SR_Q24 = 2390;

inline acid::dsp::i32 svf_f_from_hz(int hz) { return hz * TWO_PI_OVER_SR_Q24; }

inline acid::dsp::i32 decay_coeff(int tau_samples) {
    if (tau_samples < 1) tau_samples = 1;
    return acid::dsp::exp_q24(-(ONE_Q24 / tau_samples));
}

}  // namespace

Acb909Sd::Acb909Sd() {
    m_qBody = ONE_Q24 / 8;       // Q=8 (more tonal than 808)
    m_qSnap = ONE_Q24 / 3;       // Q=3
    m_fS    = svf_f_from_hz(3000);
    setTuning(static_cast<i32>(0.50 * ONE_Q24));
    setSnappy(static_cast<i32>(0.50 * ONE_Q24));
    setDecay (static_cast<i32>(0.50 * ONE_Q24));
}

void Acb909Sd::setTuning(i32 tuningQ24) {
    if (tuningQ24 < 0) tuningQ24 = 0;
    if (tuningQ24 > ONE_Q24) tuningQ24 = ONE_Q24;
    int f1 = 250 + ((150 * tuningQ24) >> Q24_SHIFT);  // 250..400
    int f2 = (f1 * 32) / 10;                          // 3.2 × f1
    m_f1 = svf_f_from_hz(f1);
    m_f2 = svf_f_from_hz(f2);
}

void Acb909Sd::setSnappy(i32 snappyQ24) {
    if (snappyQ24 < 0) snappyQ24 = 0;
    if (snappyQ24 > ONE_Q24) snappyQ24 = ONE_Q24;
    // Body never fully muted on 909 SD (different from 808).
    m_bodyGain = ONE_Q24 - mul_q24(snappyQ24, ONE_Q24 / 2);
    m_snapGain = snappyQ24;
}

void Acb909Sd::setDecay(i32 decayQ24) {
    if (decayQ24 < 0) decayQ24 = 0;
    if (decayQ24 > ONE_Q24) decayQ24 = ONE_Q24;
    constexpr int TAU_LO = 4410;   // 100 ms
    constexpr int TAU_HI = 15435;  // 350 ms
    int tau_body = TAU_LO +
        static_cast<int>(
            (static_cast<int64_t>(TAU_HI - TAU_LO) * decayQ24) >> Q24_SHIFT);
    int tau_snap = (tau_body * 35) / 100;
    m_envBodyCoeff = decay_coeff(tau_body);
    m_envSnapCoeff = decay_coeff(tau_snap);
}

void Acb909Sd::trigger(i32 velocityQ24) {
    if (velocityQ24 < 0) velocityQ24 = 0;
    if (velocityQ24 > ONE_Q24) velocityQ24 = ONE_Q24;
    m_pendingImpulse = velocityQ24 * 3;
    m_envBody = ONE_Q24;
    m_envSnap = velocityQ24;
}

acid::dsp::i16 Acb909Sd::tick() {
    i32 in = m_pendingImpulse;
    m_pendingImpulse = 0;

    m_low1 += mul_q24(m_f1, m_band1);
    i32 h1 = in - m_low1 - mul_q24(m_qBody, m_band1);
    m_band1 += mul_q24(m_f1, h1);

    m_low2 += mul_q24(m_f2, m_band2);
    i32 h2 = in - m_low2 - mul_q24(m_qBody, m_band2);
    m_band2 += mul_q24(m_f2, h2);

    i32 body = (m_band1 + m_band2) >> 1;
    body = mul_q24(body, m_envBody);
    m_envBody = mul_q24(m_envBody, m_envBodyCoeff);

    i32 n = m_noise.tick();
    m_lowS += mul_q24(m_fS, m_bandS);
    i32 hS = n - m_lowS - mul_q24(m_qSnap, m_bandS);
    m_bandS += mul_q24(m_fS, hS);
    i32 snap = mul_q24(m_bandS, m_envSnap);
    m_envSnap = mul_q24(m_envSnap, m_envSnapCoeff);

    i32 out = mul_q24(body, m_bodyGain) + mul_q24(snap, m_snapGain);

    i32 scaled = out >> (Q24_SHIFT - 14);
    return sat16(scaled);
}

}  // namespace acid::voices
