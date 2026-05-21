// cy_six_squares.cpp — see cy_six_squares.hpp.

#include "dsp/cy_six_squares.hpp"
#include <cmath>

namespace acid::dsp {

namespace {
float lerp(float lo, float hi, float t) { return lo + (hi - lo) * t; }

// Same family as HH but a different non-harmonic set so CY and HH don't
// sound identical when played in the same pattern.
constexpr float CY_BASE_FREQS[6] = {
    480.0f, 565.0f, 678.0f, 820.0f, 977.0f, 1115.0f,
};
}  // namespace

CYSixSquares::CYSixSquares(int sampleRate) : m_sampleRate(sampleRate) {
    for (int i = 0; i < 6; ++i) m_phase[i] = static_cast<u32>(0x08000000) * (i + 1);
    setParams(0.5f, 0.5f, 0.5f);
}

void CYSixSquares::setParams(float decay, float brightness, float tune) {
    float tuneMul = std::pow(2.0f, lerp(-0.5f, 0.5f, tune));
    for (int i = 0; i < 6; ++i) {
        float f = CY_BASE_FREQS[i] * tuneMul;
        float fraction = f / static_cast<float>(m_sampleRate);
        if (fraction < 0.0f) fraction = 0.0f;
        if (fraction > 0.49f) fraction = 0.49f;
        m_step[i] = static_cast<u32>(fraction * 4294967296.0);
    }

    float bMul = std::pow(2.0f, lerp(-0.5f, 0.5f, brightness));
    m_hp.setCutoff(m_sampleRate, 1500.0f * bMul, 0.7f);
    m_bp.setCutoff(m_sampleRate, 5500.0f * bMul, 2.0f);

    // Cymbal sustains much longer than HH.
    float tau = lerp(0.8f, 4.0f, decay);
    m_envCoeff = to_q24(std::exp(-1.0f / (static_cast<float>(m_sampleRate) * tau)));
}

void CYSixSquares::trigger(float velocity) {
    if (velocity < 0.0f) velocity = 0.0f;
    if (velocity > 1.0f) velocity = 1.0f;
    m_velocity = to_q24(velocity);
    m_env = to_q24(1.0f);
}

i16 CYSixSquares::tick() {
    i32 sum = 0;
    constexpr i32 SQUARE_HI = (1 << Q24_SHIFT) / 6;
    for (int i = 0; i < 6; ++i) {
        m_phase[i] += m_step[i];
        sum += (m_phase[i] & 0x80000000u) ? -SQUARE_HI : SQUARE_HI;
    }

    i32 hp = m_hp.tickHigh(sum);
    i32 bp = m_bp.tickBand(hp);

    i32 out = mul_q24(bp, m_env);
    out = mul_q24(out, m_velocity);
    m_env = mul_q24(m_env, m_envCoeff);

    i32 scaled = out >> (Q24_SHIFT - 14);
    return sat16(scaled);
}

}  // namespace acid::dsp
