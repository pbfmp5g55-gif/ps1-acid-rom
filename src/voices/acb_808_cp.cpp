// acb_808_cp.cpp — see acb_808_cp.hpp.

#include "voices/acb_808_cp.hpp"
#include "dsp/qmath.hpp"

namespace acid::voices {

using acid::dsp::mul_q24;
using acid::dsp::sat16;
using acid::dsp::Q24_SHIFT;
constexpr acid::dsp::i32 ONE_Q24 = 1 << Q24_SHIFT;

namespace {

constexpr acid::dsp::i32 TWO_PI_OVER_SR_Q24 = 2390;
constexpr uint32_t STEP_PER_HZ = 97392;

inline acid::dsp::u32 hz_to_step(int hz) {
    if (hz < 1) hz = 1;
    return static_cast<acid::dsp::u32>(hz) * STEP_PER_HZ;
}

inline acid::dsp::i32 svf_f_from_hz(int hz) {
    return hz * TWO_PI_OVER_SR_Q24;
}

inline acid::dsp::i32 decay_coeff(int tau_samples) {
    if (tau_samples < 1) tau_samples = 1;
    return acid::dsp::exp_q24(-(ONE_Q24 / tau_samples));
}

}  // namespace

Acb808Cp::Acb808Cp() {
    m_bpQ = (2 * ONE_Q24) / 5;  // 1/2.5 — open clank, not tonal
    setTuning(static_cast<i32>(0.50 * ONE_Q24));
    setDecay (static_cast<i32>(0.50 * ONE_Q24));
}

void Acb808Cp::setTuning(i32 tuningQ24) {
    if (tuningQ24 < 0) tuningQ24 = 0;
    if (tuningQ24 > ONE_Q24) tuningQ24 = ONE_Q24;
    int f1 = 400 + ((300 * tuningQ24) >> Q24_SHIFT);   // 400..700
    int f2 = (f1 * 148) / 100;                          // 1.48 × f1
    m_step1 = hz_to_step(f1);
    m_step2 = hz_to_step(f2);
    m_bpF = svf_f_from_hz((f1 + f2) / 2);
}

void Acb808Cp::setDecay(i32 decayQ24) {
    if (decayQ24 < 0) decayQ24 = 0;
    if (decayQ24 > ONE_Q24) decayQ24 = ONE_Q24;
    constexpr int TAU_LO = 3528;   // 80 ms
    constexpr int TAU_HI = 17640;  // 400 ms
    int tau = TAU_LO +
        static_cast<int>(
            (static_cast<int64_t>(TAU_HI - TAU_LO) * decayQ24) >> Q24_SHIFT);
    m_envCoeff = decay_coeff(tau);
}

void Acb808Cp::trigger(i32 velocityQ24) {
    if (velocityQ24 < 0) velocityQ24 = 0;
    if (velocityQ24 > ONE_Q24) velocityQ24 = ONE_Q24;
    m_velocity = velocityQ24;
    m_env = ONE_Q24;
}

acid::dsp::i16 Acb808Cp::tick() {
    m_phase1 += m_step1;
    m_phase2 += m_step2;

    constexpr i32 SQUARE_HI = ONE_Q24 / 2;  // ±0.5
    i32 sq1 = (m_phase1 & 0x80000000u) ? -SQUARE_HI : SQUARE_HI;
    i32 sq2 = (m_phase2 & 0x80000000u) ? -SQUARE_HI : SQUARE_HI;
    i32 in = (sq1 + sq2) >> 1;

    m_bpLow += mul_q24(m_bpF, m_bpBand);
    i32 high = in - m_bpLow - mul_q24(m_bpQ, m_bpBand);
    m_bpBand += mul_q24(m_bpF, high);

    i32 out = mul_q24(m_bpBand, m_env);
    out = mul_q24(out, m_velocity);
    m_env = mul_q24(m_env, m_envCoeff);

    i32 scaled = out >> (Q24_SHIFT - 14);
    return sat16(scaled);
}

}  // namespace acid::voices
