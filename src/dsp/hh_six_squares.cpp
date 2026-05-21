// hh_six_squares.cpp — see hh_six_squares.hpp.

#include "dsp/hh_six_squares.hpp"
#include <cmath>

namespace acid::dsp {

namespace {
float lerp(float lo, float hi, float t) { return lo + (hi - lo) * t; }

// Non-harmonic, irrational-ratio square frequencies. Picked from community
// reverse engineering writeups of the 808 cymbal generator; the exact ratios
// matter less than that they are non-integer multiples of each other (no
// shared period → metallic noise rather than tonal chord).
constexpr float HH_BASE_FREQS[6] = {
    540.0f, 619.0f, 722.0f, 892.0f, 1024.0f, 1148.0f,
};
}  // namespace

HHSixSquares::HHSixSquares(int sampleRate) : m_sampleRate(sampleRate) {
    // Stagger initial phases so the first sample isn't a 6× spike.
    for (int i = 0; i < 6; ++i) m_phase[i] = static_cast<u32>(0x10000000) * (i + 1);
    setParams(0.0f, 0.5f, 0.5f);
}

void HHSixSquares::setParams(float openness, float brightness, float tune) {
    if (openness < 0.0f) openness = 0.0f;
    if (openness > 1.0f) openness = 1.0f;

    float tuneMul = std::pow(2.0f, lerp(-0.5f, 0.5f, tune));
    for (int i = 0; i < 6; ++i) {
        float f = HH_BASE_FREQS[i] * tuneMul;
        float fraction = f / static_cast<float>(m_sampleRate);
        if (fraction < 0.0f) fraction = 0.0f;
        if (fraction > 0.49f) fraction = 0.49f;
        m_step[i] = static_cast<u32>(fraction * 4294967296.0);
    }

    // Brightness ±0.5 octave around defaults (HPF 2 kHz, BPF 6 kHz). TPT
    // SVF is stable past Nyquist so a wider range is also fine, but ±0.5
    // octave matches what's musically useful for HH tone.
    float bMul = std::pow(2.0f, lerp(-0.5f, 0.5f, brightness));
    m_hp.setCutoff(m_sampleRate, 2000.0f * bMul, 0.7f);
    m_bp.setCutoff(m_sampleRate, 6000.0f * bMul, 1.5f);

    // Closed = ~50 ms, open = ~500 ms.
    float tau = lerp(0.05f, 0.5f, openness);
    m_envCoeff = to_q24(std::exp(-1.0f / (static_cast<float>(m_sampleRate) * tau)));
}

void HHSixSquares::trigger(float velocity) {
    if (velocity < 0.0f) velocity = 0.0f;
    if (velocity > 1.0f) velocity = 1.0f;
    m_velocity = to_q24(velocity);
    m_env = to_q24(1.0f);
}

i16 HHSixSquares::tick() {
    // Sum six squares. Each contributes ±(1/6) in Q24 so the sum stays
    // bounded.
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

    // Peak Q24 ~1.0 → ~16k i16.
    i32 scaled = out >> (Q24_SHIFT - 14);
    return sat16(scaled);
}

}  // namespace acid::dsp
