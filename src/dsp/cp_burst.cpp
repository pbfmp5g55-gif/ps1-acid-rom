// cp_burst.cpp — see cp_burst.hpp.

#include "dsp/cp_burst.hpp"
#include <cmath>

namespace acid::dsp {

namespace {
float lerp(float lo, float hi, float t) { return lo + (hi - lo) * t; }

i32 svf_coeff(int sampleRate, float f0) {
    float arg = 3.14159265f * f0 / static_cast<float>(sampleRate);
    if (arg > 1.4f) arg = 1.4f;
    return to_q24(2.0f * std::sin(arg));
}

u32 freq_to_step(int sampleRate, float f) {
    float fraction = f / static_cast<float>(sampleRate);
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 0.49f) fraction = 0.49f;
    return static_cast<u32>(fraction * 4294967296.0);
}
}  // namespace

CPBurst::CPBurst(int sampleRate) : m_sampleRate(sampleRate) {
    // Slight phase offset so first sample isn't a 2× spike.
    m_phase2 = 0x40000000u;
    setParams(0.5f, 0.5f);
}

void CPBurst::setParams(float tuning, float decay) {
    float f1 = lerp(400.0f, 700.0f, tuning);
    // The 800/540 ratio (~1.48) on the real 808 is close to a perfect fifth.
    float f2 = f1 * 1.48f;

    m_step1 = freq_to_step(m_sampleRate, f1);
    m_step2 = freq_to_step(m_sampleRate, f2);

    // BPF centered between the two squares.
    m_bpF = svf_coeff(m_sampleRate, (f1 + f2) * 0.5f);
    m_bpQ = to_q24(1.0f / 2.5f);  // moderate Q — open clank rather than tonal ring

    float tau = lerp(0.08f, 0.4f, decay);
    m_envCoeff = to_q24(std::exp(-1.0f / (static_cast<float>(m_sampleRate) * tau)));
}

void CPBurst::trigger(float velocity) {
    if (velocity < 0.0f) velocity = 0.0f;
    if (velocity > 1.0f) velocity = 1.0f;
    m_velocity = to_q24(velocity);
    m_env = to_q24(1.0f);
}

i16 CPBurst::tick() {
    m_phase1 += m_step1;
    m_phase2 += m_step2;

    constexpr i32 SQUARE_HI = (1 << Q24_SHIFT) / 2;  // ±0.5 Q24 each → sum bounded ±1
    i32 sq1 = (m_phase1 & 0x80000000u) ? -SQUARE_HI : SQUARE_HI;
    i32 sq2 = (m_phase2 & 0x80000000u) ? -SQUARE_HI : SQUARE_HI;
    i32 in = (sq1 + sq2) >> 1;  // average

    m_bpLow += mul_q24(m_bpF, m_bpBand);
    i32 high = in - m_bpLow - mul_q24(m_bpQ, m_bpBand);
    m_bpBand += mul_q24(m_bpF, high);

    i32 out = mul_q24(m_bpBand, m_env);
    out = mul_q24(out, m_velocity);
    m_env = mul_q24(m_env, m_envCoeff);

    i32 scaled = out >> (Q24_SHIFT - 14);
    return sat16(scaled);
}

}  // namespace acid::dsp
