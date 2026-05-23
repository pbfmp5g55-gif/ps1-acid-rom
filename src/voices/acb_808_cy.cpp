// acb_808_cy.cpp — see acb_808_cy.hpp.

#include "voices/acb_808_cy.hpp"
#include "dsp/qmath.hpp"

namespace acid::voices {

using acid::dsp::mul_q24;
using acid::dsp::sat16;
using acid::dsp::Q24_SHIFT;
constexpr acid::dsp::i32 ONE_Q24 = 1 << Q24_SHIFT;

namespace {

constexpr acid::dsp::i32 TWO_PI_OVER_SR_Q24 = 2390;
constexpr uint32_t STEP_PER_HZ = 97392;

constexpr int CY_BASE_FREQS[6] = {480, 565, 678, 820, 977, 1115};

inline acid::dsp::u32 hz_to_step(int hz) {
    if (hz < 1) hz = 1;
    return static_cast<acid::dsp::u32>(hz) * STEP_PER_HZ;
}

inline acid::dsp::i32 svf_f_from_hz(int hz) { return hz * TWO_PI_OVER_SR_Q24; }

inline acid::dsp::i32 decay_coeff(int tau_samples) {
    if (tau_samples < 1) tau_samples = 1;
    return acid::dsp::exp_q24(-(ONE_Q24 / tau_samples));
}

inline acid::dsp::i32 plus_minus_half_octave_mult_q24(acid::dsp::i32 tQ24) {
    if (tQ24 < 0) tQ24 = 0;
    if (tQ24 > ONE_Q24) tQ24 = ONE_Q24;
    acid::dsp::i32 pw = acid::dsp::pow2_unit_q24(tQ24);
    constexpr acid::dsp::i32 INV_SQRT2 = static_cast<acid::dsp::i32>(0.70710678 * ONE_Q24);
    return mul_q24(pw, INV_SQRT2);
}

}  // namespace

Acb808Cy::Acb808Cy() {
    for (int i = 0; i < 6; ++i) m_phase[i] = static_cast<u32>(0x08000000) * (i + 1);
    m_hp_q = static_cast<i32>(ONE_Q24 / 0.7);
    m_bp_q = static_cast<i32>(ONE_Q24 / 2.0);   // Q=2.0, slightly narrower BPF
    setTune     (ONE_Q24 / 2);
    setBrightness(ONE_Q24 / 2);
    setDecay    (ONE_Q24 / 2);
}

void Acb808Cy::setDecay(i32 decayQ24) {
    if (decayQ24 < 0) decayQ24 = 0;
    if (decayQ24 > ONE_Q24) decayQ24 = ONE_Q24;
    // Cymbal sustains much longer than HH: 0.8..4.0 s.
    constexpr int TAU_LO = 35280;     // 0.8 s
    constexpr int TAU_HI = 176400;    // 4.0 s
    int tau = TAU_LO +
        static_cast<int>(
            (static_cast<int64_t>(TAU_HI - TAU_LO) * decayQ24) >> Q24_SHIFT);
    m_envCoeff = decay_coeff(tau);
}

void Acb808Cy::setBrightness(i32 brightQ24) {
    if (brightQ24 < 0) brightQ24 = 0;
    if (brightQ24 > ONE_Q24) brightQ24 = ONE_Q24;
    i32 mul = plus_minus_half_octave_mult_q24(brightQ24);
    int hpHz = static_cast<int>((static_cast<int64_t>(1500) * mul) >> Q24_SHIFT);
    int bpHz = static_cast<int>((static_cast<int64_t>(5500) * mul) >> Q24_SHIFT);
    m_hp_f = svf_f_from_hz(hpHz);
    m_bp_f = svf_f_from_hz(bpHz);
}

void Acb808Cy::setTune(i32 tuneQ24) {
    if (tuneQ24 < 0) tuneQ24 = 0;
    if (tuneQ24 > ONE_Q24) tuneQ24 = ONE_Q24;
    i32 mul = plus_minus_half_octave_mult_q24(tuneQ24);
    for (int i = 0; i < 6; ++i) {
        int hz = static_cast<int>(
            (static_cast<int64_t>(CY_BASE_FREQS[i]) * mul) >> Q24_SHIFT);
        m_step[i] = hz_to_step(hz);
    }
}

void Acb808Cy::trigger(i32 velocityQ24) {
    if (velocityQ24 < 0) velocityQ24 = 0;
    if (velocityQ24 > ONE_Q24) velocityQ24 = ONE_Q24;
    m_velocity = velocityQ24;
    m_env = ONE_Q24;
}

acid::dsp::i16 Acb808Cy::tick() {
    i32 sum = 0;
    constexpr i32 SQUARE_HI = ONE_Q24 / 6;
    for (int i = 0; i < 6; ++i) {
        m_phase[i] += m_step[i];
        sum += (m_phase[i] & 0x80000000u) ? -SQUARE_HI : SQUARE_HI;
    }

    m_hp_low += mul_q24(m_hp_f, m_hp_band);
    i32 hp = sum - m_hp_low - mul_q24(m_hp_q, m_hp_band);
    m_hp_band += mul_q24(m_hp_f, hp);

    m_bp_low += mul_q24(m_bp_f, m_bp_band);
    i32 bp_high = hp - m_bp_low - mul_q24(m_bp_q, m_bp_band);
    m_bp_band += mul_q24(m_bp_f, bp_high);
    i32 bp = m_bp_band;

    i32 out = mul_q24(bp, m_env);
    out = mul_q24(out, m_velocity);
    m_env = mul_q24(m_env, m_envCoeff);

    i32 scaled = out >> (Q24_SHIFT - 14);
    return sat16(scaled);
}

}  // namespace acid::voices
