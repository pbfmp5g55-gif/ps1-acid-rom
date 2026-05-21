// tom_tbridge.cpp — see tom_tbridge.hpp.

#include "dsp/tom_tbridge.hpp"
#include <cmath>

namespace acid::dsp {

namespace {
float lerp(float lo, float hi, float t) { return lo + (hi - lo) * t; }
}

TomTBridge::TomTBridge(int sampleRate) : m_sampleRate(sampleRate) { setParams(0.5f, 0.5f, 0.5f); }

void TomTBridge::setParams(float tuning, float tone, float decay) {
    float f0 = lerp(70.0f, 250.0f, tuning);
    float f = 2.0f * std::sin(3.14159265f * f0 / static_cast<float>(m_sampleRate));
    m_f = to_q24(f);

    float Q = lerp(4.0f, 16.0f, tone);
    m_q = to_q24(1.0f / Q);

    float tau = lerp(0.05f, 0.7f, decay);
    m_envCoeff = to_q24(std::exp(-1.0f / (static_cast<float>(m_sampleRate) * tau)));
}

void TomTBridge::trigger(float velocity) {
    if (velocity < 0.0f) velocity = 0.0f;
    if (velocity > 1.0f) velocity = 1.0f;
    // Higher f0 than BD so ringup reaches usable amplitude with smaller
    // impulse. 4.0 lands -3 dBFS-ish at v=1 with default Q.
    m_pendingImpulse = to_q24(velocity * 4.0f);
    m_env = to_q24(1.0f);
}

i16 TomTBridge::tick() {
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

}  // namespace acid::dsp
