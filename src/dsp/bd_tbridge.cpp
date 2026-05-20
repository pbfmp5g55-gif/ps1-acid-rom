// bd_tbridge.cpp — implementation. See bd_tbridge.hpp for topology notes.

#include "dsp/bd_tbridge.hpp"
#include <cmath>

namespace acid::dsp {

namespace {

// Map UI pot positions to physical coefficients. Picked by ear against
// community recordings of an original TR-808 BD voice; refine in M3
// against the test render comparison.
float lerp(float lo, float hi, float t) { return lo + (hi - lo) * t; }

}  // namespace

BDTBridge::BDTBridge(int sampleRate) : m_sampleRate(sampleRate) { setParams(0.5f, 0.6f, 0.5f); }

void BDTBridge::setParams(float tone, float decay, float tuning) {
    // f0 in Hz → SVF coefficient.
    float f0 = lerp(38.0f, 85.0f, tuning);
    // Chamberlin SVF stability requires f < sr/pi roughly; safe here.
    float f = 2.0f * std::sin(3.14159265f * f0 / static_cast<float>(m_sampleRate));
    m_f = to_q24(f);

    // Tank Q: tone pot raises Q. Lower q-coeff = higher resonance.
    float Q = lerp(2.0f, 40.0f, tone);
    m_q = to_q24(1.0f / Q);

    // Decay env: exp(-1/(sr * tau)). tau in seconds.
    float tau = lerp(0.04f, 1.5f, decay);
    float coeff = std::exp(-1.0f / (static_cast<float>(m_sampleRate) * tau));
    m_envCoeff = to_q24(coeff);
}

void BDTBridge::trigger(float velocity) {
    if (velocity < 0.0f) velocity = 0.0f;
    if (velocity > 1.0f) velocity = 1.0f;
    // The T-bridge tank rings up over ~SR/f0 samples (~360 samples at 61 Hz /
    // 22 kHz) reaching peak amplitude ~ f/q_coeff times the impulse. With the
    // small f coefficient required to hit sub-bass frequencies, we need a
    // big impulse to make the BP output reach a useful audio amplitude
    // without saturating later. 12.0 was picked so a velocity=1 hit peaks
    // near -3 dBFS on its own after the ringup completes.
    m_pendingImpulse = to_q24(velocity * 12.0f);
    m_env = to_q24(1.0f);
}

i16 BDTBridge::tick() {
    // SVF (Chamberlin):
    //   low  += f * band
    //   high  = in - low - q * band   (we use in = impulse, then 0)
    //   band += f * high
    // The band-pass output (band) is the resonator we monitor; it's the
    // closest analogue to the T-bridge's natural mode.
    i32 in = m_pendingImpulse;
    m_pendingImpulse = 0;

    m_low += mul_q24(m_f, m_band);
    i32 high = in - m_low - mul_q24(m_q, m_band);
    m_band += mul_q24(m_f, high);

    // Apply amplitude envelope (post-resonator, like the VCA in the real
    // circuit's output stage).
    i32 out = mul_q24(m_band, m_env);
    m_env = mul_q24(m_env, m_envCoeff);

    // Scale Q24 → i16 with conservative headroom (×16384 ≈ Q14 audio domain).
    i32 scaled = out >> (Q24_SHIFT - 14);
    return sat16(scaled);
}

}  // namespace acid::dsp
