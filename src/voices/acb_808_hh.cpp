// acb_808_hh.cpp — see acb_808_hh.hpp.

#include "voices/acb_808_hh.hpp"
#include "dsp/qmath.hpp"

namespace acid::voices {

using acid::dsp::mul_q24;
using acid::dsp::sat16;
using acid::dsp::Q24_SHIFT;
constexpr acid::dsp::i32 ONE_Q24 = 1 << Q24_SHIFT;

namespace {

constexpr acid::dsp::i32 TWO_PI_OVER_SR_Q24 = 2390;
constexpr uint32_t STEP_PER_HZ = 97392;

// 808 HH base freqs — irrational ratios so the sum is metallic noise
// (no common periodicity).
constexpr int HH_BASE_FREQS[6] = {540, 619, 722, 892, 1024, 1148};

inline acid::dsp::u32 hz_to_step(int hz) {
    if (hz < 1) hz = 1;
    return static_cast<acid::dsp::u32>(hz) * STEP_PER_HZ;
}

inline acid::dsp::i32 svf_f_from_hz(int hz) { return hz * TWO_PI_OVER_SR_Q24; }

inline acid::dsp::i32 decay_coeff(int tau_samples) {
    if (tau_samples < 1) tau_samples = 1;
    return acid::dsp::exp_q24(-(ONE_Q24 / tau_samples));
}

// tuneQ24 / brightQ24 ∈ [0, ONE] map to ±0.5 octaves: mul = 2^(t - 0.5).
inline acid::dsp::i32 plus_minus_half_octave_mult_q24(acid::dsp::i32 tQ24) {
    if (tQ24 < 0) tQ24 = 0;
    if (tQ24 > ONE_Q24) tQ24 = ONE_Q24;
    // Map [0, ONE] → [-0.5, +0.5] octaves → 2^(...) = pow2_unit(t) / sqrt(2)
    // = pow2(t) * 0.7071. We compute as: ratio = pow2(t) * 23170 / 32768.
    acid::dsp::i32 pw = acid::dsp::pow2_unit_q24(tQ24);  // [ONE, 2*ONE]
    // Multiply by 1/sqrt(2) ≈ 0.7071 (Q24).
    constexpr acid::dsp::i32 INV_SQRT2 = static_cast<acid::dsp::i32>(0.70710678 * ONE_Q24);
    return mul_q24(pw, INV_SQRT2);
}

}  // namespace

Acb808Hh::Acb808Hh() {
    // Stagger initial phases so the first sample isn't a 6× spike.
    for (int i = 0; i < 6; ++i) m_phase[i] = static_cast<u32>(0x10000000) * (i + 1);
    m_hp_q = static_cast<i32>(ONE_Q24 / 0.7);   // 1/Q with Q=0.7 ≈ 1.43
    m_bp_q = static_cast<i32>(ONE_Q24 / 1.5);   // Q=1.5 ≈ 0.667
    setTune     (ONE_Q24 / 2);
    setBrightness(ONE_Q24 / 2);
    setOpenness (0);  // closed by default
}

void Acb808Hh::setOpenness(i32 opennessQ24) {
    if (opennessQ24 < 0) opennessQ24 = 0;
    if (opennessQ24 > ONE_Q24) opennessQ24 = ONE_Q24;
    constexpr int TAU_LO = 2205;     // 50 ms
    constexpr int TAU_HI = 22050;    // 500 ms
    int tau = TAU_LO +
        static_cast<int>(
            (static_cast<int64_t>(TAU_HI - TAU_LO) * opennessQ24) >> Q24_SHIFT);
    m_envCoeff = decay_coeff(tau);
}

void Acb808Hh::setBrightness(i32 brightQ24) {
    if (brightQ24 < 0) brightQ24 = 0;
    if (brightQ24 > ONE_Q24) brightQ24 = ONE_Q24;
    i32 mul = plus_minus_half_octave_mult_q24(brightQ24);
    // HPF base 2 kHz, BPF base 6 kHz; multiply each.
    int hpHz = static_cast<int>((static_cast<int64_t>(2000) * mul) >> Q24_SHIFT);
    int bpHz = static_cast<int>((static_cast<int64_t>(6000) * mul) >> Q24_SHIFT);
    m_hp_f = svf_f_from_hz(hpHz);
    m_bp_f = svf_f_from_hz(bpHz);
}

void Acb808Hh::setTune(i32 tuneQ24) {
    if (tuneQ24 < 0) tuneQ24 = 0;
    if (tuneQ24 > ONE_Q24) tuneQ24 = ONE_Q24;
    i32 mul = plus_minus_half_octave_mult_q24(tuneQ24);
    for (int i = 0; i < 6; ++i) {
        int hz = static_cast<int>(
            (static_cast<int64_t>(HH_BASE_FREQS[i]) * mul) >> Q24_SHIFT);
        m_step[i] = hz_to_step(hz);
    }
}

void Acb808Hh::trigger(i32 velocityQ24) {
    if (velocityQ24 < 0) velocityQ24 = 0;
    if (velocityQ24 > ONE_Q24) velocityQ24 = ONE_Q24;
    m_velocity = velocityQ24;
    m_env = ONE_Q24;
}

acid::dsp::i16 Acb808Hh::tick() {
    // Sum six squares, each ±1/6 in Q24.
    i32 sum = 0;
    constexpr i32 SQUARE_HI = ONE_Q24 / 6;
    for (int i = 0; i < 6; ++i) {
        m_phase[i] += m_step[i];
        sum += (m_phase[i] & 0x80000000u) ? -SQUARE_HI : SQUARE_HI;
    }

    // HPF Chamberlin (we listen to the high tap).
    m_hp_low += mul_q24(m_hp_f, m_hp_band);
    i32 hp = sum - m_hp_low - mul_q24(m_hp_q, m_hp_band);
    m_hp_band += mul_q24(m_hp_f, hp);

    // BPF Chamberlin (band tap).
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
