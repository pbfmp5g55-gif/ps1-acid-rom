// acb_808_bd.cpp — see acb_808_bd.hpp.

#include "voices/acb_808_bd.hpp"
#include "dsp/qmath.hpp"

namespace acid::voices {

using acid::dsp::mul_q24;
using acid::dsp::sat16;
using acid::dsp::Q24_SHIFT;
constexpr acid::dsp::i32 ONE_Q24 = 1 << Q24_SHIFT;

namespace {

constexpr acid::dsp::i32 TWO_PI_OVER_SR_Q24 = 2390;  // 2π / 44100 * 2^24

inline acid::dsp::i32 lerp_q24(acid::dsp::i32 a, acid::dsp::i32 b,
                               acid::dsp::i32 t) {
    return a + mul_q24(b - a, t);
}

// 2π * f0_hz / SR small-angle approx, returns Q24. Same shortcut as
// acb_tb303_stage1; f0 is well below Nyquist here (38..85 Hz) so the
// error is <0.001 %, well below audible.
inline acid::dsp::i32 svf_f_from_hz(int f0_hz) {
    return f0_hz * TWO_PI_OVER_SR_Q24;
}

// Map decayParam (Q24 0..1) → env coefficient (Q24).
// tau_samples ∈ [1764, 66150] for the BD's longer-than-303 decay range.
inline acid::dsp::i32 decay_param_to_coeff(acid::dsp::i32 decayParamQ24) {
    constexpr int32_t TAU_LO = 1764;   // 0.04 * 44100
    constexpr int32_t TAU_HI = 66150;  // 1.50 * 44100
    int32_t tau = TAU_LO +
                  static_cast<int32_t>(
                      (static_cast<int64_t>(TAU_HI - TAU_LO) * decayParamQ24) >>
                      Q24_SHIFT);
    if (tau < 1) tau = 1;
    return acid::dsp::exp_q24(-(ONE_Q24 / tau));
}

}  // namespace

Acb808Bd::Acb808Bd() {
    setTuning(static_cast<i32>(0.50 * ONE_Q24));
    setTone  (static_cast<i32>(0.50 * ONE_Q24));
    setDecay (static_cast<i32>(0.60 * ONE_Q24));
}

void Acb808Bd::setTuning(i32 tuningQ24) {
    if (tuningQ24 < 0) tuningQ24 = 0;
    if (tuningQ24 > ONE_Q24) tuningQ24 = ONE_Q24;
    // 38..85 Hz linear
    int f0_hz = 38 + ((47 * tuningQ24) >> Q24_SHIFT);
    m_f = svf_f_from_hz(f0_hz);
}

void Acb808Bd::setTone(i32 toneQ24) {
    if (toneQ24 < 0) toneQ24 = 0;
    if (toneQ24 > ONE_Q24) toneQ24 = ONE_Q24;
    // Q 2..40 → 1/Q in [1/2, 1/40] Q24.
    constexpr i32 INV_Q_LO = ONE_Q24 / 2;   // Q=2
    constexpr i32 INV_Q_HI = ONE_Q24 / 40;  // Q=40
    m_q = lerp_q24(INV_Q_LO, INV_Q_HI, toneQ24);
}

void Acb808Bd::setDecay(i32 decayQ24) {
    if (decayQ24 < 0) decayQ24 = 0;
    if (decayQ24 > ONE_Q24) decayQ24 = ONE_Q24;
    m_envCoeff = decay_param_to_coeff(decayQ24);
}

void Acb808Bd::trigger(i32 velocityQ24) {
    if (velocityQ24 < 0) velocityQ24 = 0;
    if (velocityQ24 > ONE_Q24) velocityQ24 = ONE_Q24;
    // Big impulse (×12) so the low-f0 tank ringup reaches audible amplitude.
    // velocity * 12 in Q24 = mul_q24(velocity, 12*ONE).
    m_pendingImpulse = velocityQ24 * 12;  // i32 OK: max 12*ONE_Q24 ≈ 2^27
    m_env = ONE_Q24;
}

acid::dsp::i16 Acb808Bd::tick() {
    // Chamberlin SVF — band-pass output is the analogue of the T-bridge's
    // natural mode.
    i32 in = m_pendingImpulse;
    m_pendingImpulse = 0;

    m_low += mul_q24(m_f, m_band);
    i32 high = in - m_low - mul_q24(m_q, m_band);
    m_band += mul_q24(m_f, high);

    // Apply amplitude envelope (post-resonator, like the VCA in the real
    // circuit's output stage).
    i32 out = mul_q24(m_band, m_env);
    m_env = mul_q24(m_env, m_envCoeff);

    // Q24 → i16 with +6 dB boost vs host headroom for live-mix audibility.
    i32 scaled = out >> (Q24_SHIFT - 15);
    return sat16(scaled);
}

}  // namespace acid::voices
