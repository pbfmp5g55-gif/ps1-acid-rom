// r909_sd.cpp — see r909_sd.hpp.

#include "dsp/r909_sd.hpp"
#include <cmath>

namespace acid::dsp {

namespace {
float lerp(float lo, float hi, float t) { return lo + (hi - lo) * t; }

i32 svf_coeff(int sampleRate, float f0) {
    float arg = 3.14159265f * f0 / static_cast<float>(sampleRate);
    if (arg > 1.0f) arg = 1.0f;
    return to_q24(2.0f * std::sin(arg));
}
}  // namespace

R909SD::R909SD(int sampleRate) : m_sampleRate(sampleRate) { setParams(0.5f, 0.5f, 0.5f); }

void R909SD::setParams(float tuning, float snappy, float decay) {
    // 909 SD body is a two-band twin-T pair: low at ~340 Hz, high at ~1100.
    float f1 = lerp(250.0f, 400.0f, tuning);
    float f2 = f1 * 3.2f;  // upper partner ratio (909 schematic ~ 1100/340)
    m_f1 = svf_coeff(m_sampleRate, f1);
    m_f2 = svf_coeff(m_sampleRate, f2);
    m_qBody = to_q24(1.0f / 8.0f);  // higher Q than 808 SD — more tonal

    // Snap BPF center ~3 kHz, narrow Q so the click cuts through.
    m_fS    = svf_coeff(m_sampleRate, 3000.0f);
    m_qSnap = to_q24(1.0f / 3.0f);

    float tauBody = lerp(0.10f, 0.35f, decay);
    float tauSnap = tauBody * 0.35f;  // snap is much shorter than body
    m_envBodyCoeff = to_q24(std::exp(-1.0f / (static_cast<float>(m_sampleRate) * tauBody)));
    m_envSnapCoeff = to_q24(std::exp(-1.0f / (static_cast<float>(m_sampleRate) * tauSnap)));

    if (snappy < 0.0f) snappy = 0.0f;
    if (snappy > 1.0f) snappy = 1.0f;
    m_bodyGain = to_q24(1.0f - snappy * 0.5f);  // body never fully muted
    m_snapGain = to_q24(snappy);
}

void R909SD::trigger(float velocity) {
    if (velocity < 0.0f) velocity = 0.0f;
    if (velocity > 1.0f) velocity = 1.0f;
    m_pendingImpulse = to_q24(velocity * 3.0f);
    m_envBody = to_q24(1.0f);
    m_envSnap = to_q24(velocity);
}

i16 R909SD::tick() {
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

    // Snap path.
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

}  // namespace acid::dsp
