// cb_tbridge.cpp — see cb_tbridge.hpp.

#include "dsp/cb_tbridge.hpp"
#include <cmath>

namespace acid::dsp {

namespace {
float lerp(float lo, float hi, float t) { return lo + (hi - lo) * t; }
}

CBTBridge::CBTBridge(int sampleRate) : m_sampleRate(sampleRate) { setParams(0.5f, 0.5f, 0.5f); }

void CBTBridge::setParams(float tuning, float tone, float decay) {
    float f0 = lerp(1500.0f, 3500.0f, tuning);
    // Chamberlin stability — clamp arg of sin to <pi/2.
    float arg = 3.14159265f * f0 / static_cast<float>(m_sampleRate);
    if (arg > 1.4f) arg = 1.4f;
    m_f = to_q24(2.0f * std::sin(arg));

    float Q = lerp(6.0f, 20.0f, tone);
    m_q = to_q24(1.0f / Q);

    float tau = lerp(0.02f, 0.12f, decay);
    m_envCoeff = to_q24(std::exp(-1.0f / (static_cast<float>(m_sampleRate) * tau)));
}

void CBTBridge::trigger(float velocity) {
    if (velocity < 0.0f) velocity = 0.0f;
    if (velocity > 1.0f) velocity = 1.0f;
    // Much higher f0 than BD/Toms — ringup is fast and reaches good amplitude
    // with a unity-scale impulse. 1.5 lands near -3 dBFS at v=1 with default Q.
    m_pendingImpulse = to_q24(velocity * 1.5f);
    m_env = to_q24(1.0f);
}

i16 CBTBridge::tick() {
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
