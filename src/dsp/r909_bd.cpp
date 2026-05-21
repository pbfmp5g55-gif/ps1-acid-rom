// r909_bd.cpp — see r909_bd.hpp.

#include "dsp/r909_bd.hpp"
#include <cmath>

namespace acid::dsp {

namespace {
float lerp(float lo, float hi, float t) { return lo + (hi - lo) * t; }
}

R909BD::R909BD(int sampleRate) : m_sampleRate(sampleRate) {
    setParams(0.5f, 0.5f, 0.6f, 0.5f);
}

void R909BD::setParams(float tuning, float tone, float decay, float attack) {
    // 909 BD sits a touch higher than 808 — 55..110 Hz vs 808's 38..85.
    float f0 = lerp(55.0f, 110.0f, tuning);
    float f  = 2.0f * std::sin(3.14159265f * f0 / static_cast<float>(m_sampleRate));
    m_f = to_q24(f);

    // Tone range similar to 808 BD; 909 tank Q is slightly higher.
    float Q = lerp(4.0f, 30.0f, tone);
    m_q = to_q24(1.0f / Q);

    float tau = lerp(0.08f, 1.2f, decay);
    m_envCoeff = to_q24(std::exp(-1.0f / (static_cast<float>(m_sampleRate) * tau)));

    // Click is a fast (~5 ms) decay on a noise burst. attack=0 mutes it.
    if (attack < 0.0f) attack = 0.0f;
    if (attack > 1.0f) attack = 1.0f;
    m_clickGain = to_q24(attack * 0.6f);  // 0.6 = mix level vs body
    float clickTau = 0.005f;  // 5 ms
    m_clickEnvCoeff = to_q24(std::exp(-1.0f / (static_cast<float>(m_sampleRate) * clickTau)));
}

void R909BD::trigger(float velocity) {
    if (velocity < 0.0f) velocity = 0.0f;
    if (velocity > 1.0f) velocity = 1.0f;
    // Slightly bigger impulse than 808 BD — 909 hits harder.
    m_pendingImpulse = to_q24(velocity * 14.0f);
    m_env = to_q24(1.0f);
    m_clickEnv = to_q24(velocity);
}

i16 R909BD::tick() {
    i32 in = m_pendingImpulse;
    m_pendingImpulse = 0;

    m_low += mul_q24(m_f, m_band);
    i32 high = in - m_low - mul_q24(m_q, m_band);
    m_band += mul_q24(m_f, high);

    i32 body = mul_q24(m_band, m_env);
    m_env = mul_q24(m_env, m_envCoeff);

    // Click: noise * clickGain * clickEnv. clickGain bakes in the user-set
    // attack amount; clickEnv decays per sample.
    i32 click = mul_q24(m_noise.tick(), m_clickGain);
    click = mul_q24(click, m_clickEnv);
    m_clickEnv = mul_q24(m_clickEnv, m_clickEnvCoeff);

    i32 out = body + click;

    i32 scaled = out >> (Q24_SHIFT - 14);
    return sat16(scaled);
}

}  // namespace acid::dsp
