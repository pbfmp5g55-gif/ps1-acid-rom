// sd_twint.cpp — implementation. See sd_twint.hpp for topology notes.

#include "dsp/sd_twint.hpp"
#include <cmath>

namespace acid::dsp {

namespace {
float lerp(float lo, float hi, float t) { return lo + (hi - lo) * t; }

i32 svf_coeff(int sampleRate, float f0) {
    float f = 2.0f * std::sin(3.14159265f * f0 / static_cast<float>(sampleRate));
    return to_q24(f);
}
}  // namespace

SDTwinT::SDTwinT(int sampleRate) : m_sampleRate(sampleRate) { setParams(0.5f, 0.5f, 0.5f); }

void SDTwinT::setParams(float tuning, float snappy, float decay) {
    // Body resonator pair. Two slightly detuned modes give the snare its
    // pitched-but-not-tonal character. Real 808 uses ~185 Hz and ~330 Hz.
    float f1 = lerp(150.0f, 250.0f, tuning);
    float f2 = f1 * 1.78f;  // detuned partner, fixed ratio
    m_f1 = svf_coeff(m_sampleRate, f1);
    m_f2 = svf_coeff(m_sampleRate, f2);

    // Body Q ~6 — pitched but loses energy quickly. Lower than BD's tank.
    m_qBody = to_q24(1.0f / 6.0f);

    // Snappy BPF center ~2 kHz, broad Q (~1.5).
    m_fS = svf_coeff(m_sampleRate, 2000.0f);
    m_qSnap = to_q24(1.0f / 1.5f);

    // Env time constants (seconds). Snap shorter than body.
    float tauBody = lerp(0.08f, 0.4f, decay);
    float tauSnap = tauBody * 0.5f;
    m_envBodyCoeff = to_q24(std::exp(-1.0f / (static_cast<float>(m_sampleRate) * tauBody)));
    m_envSnapCoeff = to_q24(std::exp(-1.0f / (static_cast<float>(m_sampleRate) * tauSnap)));

    if (snappy < 0.0f) snappy = 0.0f;
    if (snappy > 1.0f) snappy = 1.0f;
    m_bodyGain = to_q24(1.0f - snappy);
    m_snapGain = to_q24(snappy);
}

void SDTwinT::trigger(float velocity) {
    if (velocity < 0.0f) velocity = 0.0f;
    if (velocity > 1.0f) velocity = 1.0f;
    // Body resonators are higher-freq than BD so smaller pre-multiplier is
    // enough to reach a useful amplitude. ~3.0 hits comfortably loud at v=1.
    m_pendingImpulse = to_q24(velocity * 3.0f);
    // Snap path runs noise straight (no impulse), so scale its env start by
    // velocity so peak amplitude tracks the body path.
    m_envBody = to_q24(1.0f);
    m_envSnap = to_q24(velocity);
}

i16 SDTwinT::tick() {
    // ---- Body: two SVFs in parallel, summed.
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

    // ---- Snap: noise → BPF → env.
    i32 n = m_noise.tick();
    m_lowS += mul_q24(m_fS, m_bandS);
    i32 highS = n - m_lowS - mul_q24(m_qSnap, m_bandS);
    m_bandS += mul_q24(m_fS, highS);
    i32 snap = mul_q24(m_bandS, m_envSnap);
    m_envSnap = mul_q24(m_envSnap, m_envSnapCoeff);

    // Mix.
    i32 out = mul_q24(body, m_bodyGain) + mul_q24(snap, m_snapGain);

    // Q24 → i16. Body path uses ×3 input so net gain is similar to BD; use
    // the same headroom shift.
    i32 scaled = out >> (Q24_SHIFT - 14);
    return sat16(scaled);
}

}  // namespace acid::dsp
