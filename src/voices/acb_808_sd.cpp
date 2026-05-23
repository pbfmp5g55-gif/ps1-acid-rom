// acb_808_sd.cpp — see acb_808_sd.hpp.

#include "voices/acb_808_sd.hpp"
#include "dsp/qmath.hpp"

namespace acid::voices {

using acid::dsp::mul_q24;
using acid::dsp::sat16;
using acid::dsp::Q24_SHIFT;
constexpr acid::dsp::i32 ONE_Q24 = 1 << Q24_SHIFT;

namespace {

constexpr acid::dsp::i32 TWO_PI_OVER_SR_Q24 = 2390;

inline acid::dsp::i32 svf_f_from_hz(int f0_hz) {
    return f0_hz * TWO_PI_OVER_SR_Q24;
}

inline acid::dsp::i32 decay_to_coeff(int tau_samples) {
    if (tau_samples < 1) tau_samples = 1;
    return acid::dsp::exp_q24(-(ONE_Q24 / tau_samples));
}

}  // namespace

Acb808Sd::Acb808Sd() {
    setTuning(static_cast<i32>(0.50 * ONE_Q24));
    setSnappy(static_cast<i32>(0.50 * ONE_Q24));
    setDecay (static_cast<i32>(0.50 * ONE_Q24));
    // Snappy BPF center ~2 kHz with Q ~1.5 — set once and forget.
    m_fS    = svf_f_from_hz(2000);
    m_qSnap = (2 * ONE_Q24) / 3;       // 1/1.5
    m_qBody = ONE_Q24 / 6;             // 1/Q with Q=6
}

void Acb808Sd::setTuning(i32 tuningQ24) {
    if (tuningQ24 < 0) tuningQ24 = 0;
    if (tuningQ24 > ONE_Q24) tuningQ24 = ONE_Q24;
    // f1 ∈ [150, 250] Hz, f2 = 1.78 * f1.
    int f1_hz = 150 + ((100 * tuningQ24) >> Q24_SHIFT);
    int f2_hz = (f1_hz * 178) / 100;  // 1.78 * f1 in int
    m_f1 = svf_f_from_hz(f1_hz);
    m_f2 = svf_f_from_hz(f2_hz);
}

void Acb808Sd::setSnappy(i32 snappyQ24) {
    if (snappyQ24 < 0) snappyQ24 = 0;
    if (snappyQ24 > ONE_Q24) snappyQ24 = ONE_Q24;
    m_bodyGain = ONE_Q24 - snappyQ24;
    m_snapGain = snappyQ24;
}

void Acb808Sd::setDecay(i32 decayQ24) {
    if (decayQ24 < 0) decayQ24 = 0;
    if (decayQ24 > ONE_Q24) decayQ24 = ONE_Q24;
    // tauBody ∈ [80 ms, 400 ms] linear in Q24 → samples ∈ [3528, 17640].
    constexpr int TAU_LO = 3528;
    constexpr int TAU_HI = 17640;
    int tau_body = TAU_LO +
        static_cast<int>(
            (static_cast<int64_t>(TAU_HI - TAU_LO) * decayQ24) >> Q24_SHIFT);
    int tau_snap = tau_body / 2;
    m_envBodyCoeff = decay_to_coeff(tau_body);
    m_envSnapCoeff = decay_to_coeff(tau_snap);
}

void Acb808Sd::trigger(i32 velocityQ24) {
    if (velocityQ24 < 0) velocityQ24 = 0;
    if (velocityQ24 > ONE_Q24) velocityQ24 = ONE_Q24;
    // Body resonators are higher-freq than BD so smaller pre-multiplier
    // (×3) is enough.
    m_pendingImpulse = velocityQ24 * 3;
    m_envBody = ONE_Q24;
    m_envSnap = velocityQ24;
}

acid::dsp::i16 Acb808Sd::tick() {
    // Body: two parallel SVFs summed.
    i32 in = m_pendingImpulse;
    m_pendingImpulse = 0;

    m_low1 += mul_q24(m_f1, m_band1);
    i32 high1 = in - m_low1 - mul_q24(m_qBody, m_band1);
    m_band1 += mul_q24(m_f1, high1);

    m_low2 += mul_q24(m_f2, m_band2);
    i32 high2 = in - m_low2 - mul_q24(m_qBody, m_band2);
    m_band2 += mul_q24(m_f2, high2);

    i32 body = (m_band1 + m_band2) >> 1;
    body = mul_q24(body, m_envBody);
    m_envBody = mul_q24(m_envBody, m_envBodyCoeff);

    // Snap: noise → BPF → env.
    i32 n = m_noise.tick();
    m_lowS += mul_q24(m_fS, m_bandS);
    i32 highS = n - m_lowS - mul_q24(m_qSnap, m_bandS);
    m_bandS += mul_q24(m_fS, highS);
    i32 snap = mul_q24(m_bandS, m_envSnap);
    m_envSnap = mul_q24(m_envSnap, m_envSnapCoeff);

    // Mix.
    i32 out = mul_q24(body, m_bodyGain) + mul_q24(snap, m_snapGain);

    i32 scaled = out >> (Q24_SHIFT - 14);
    return sat16(scaled);
}

}  // namespace acid::voices
