// acb_tb303_stage1.cpp — see acb_tb303_stage1.hpp.
//
// All transcendental math goes through qmath LUTs so this compiles cleanly
// on the PS1 mips toolchain (no libm). Float-domain coefficient mapping is
// emulated with Q24 piecewise-linear / LUT-based functions.

#include "voices/acb_tb303_stage1.hpp"
#include "dsp/qmath.hpp"

namespace acid::voices {

using acid::dsp::mul_q24;
using acid::dsp::sat16;
using acid::dsp::Q24_SHIFT;
constexpr i32 ONE_Q24 = 1 << Q24_SHIFT;

namespace {

// freq → step (phase increment): step = freq / SR * 2^32 = freq * (2^32 / SR).
// 2^32 / 44100 ≈ 97391.5 (Q0). We use Q8 to avoid losing the .5 bit.
constexpr uint32_t STEP_PER_HZ_Q8 = (4294967296ull * 256ull) / 44100ull;  // ≈ 24932235

inline u32 hz_q24_to_step(i32 hzQ24) {
    // hzQ24 is Q24, but for audible frequencies (20..20000) the integer
    // part fits in 14 bits — multiply by STEP_PER_HZ_Q8 in 64-bit, shift down.
    int64_t h = static_cast<int64_t>(hzQ24);             // Q24
    int64_t s = (h * static_cast<int64_t>(STEP_PER_HZ_Q8)) >> (24 + 8);
    return static_cast<u32>(s);
}

// Chamberlin SVF f coefficient = 2 * sin(π * fc / SR). We feed fc/SR as a
// uint32 normalized phase fragment (0..0x80000000 = 0..π), look up sin via
// qmath, then double.
inline i32 svf_f_from_norm(uint32_t fcOverSrQ31) {
    // fcOverSrQ31 maps 0..0x80000000 → 0..0.5 (Nyquist). We want sin(π*x):
    //   π*x maps 0..π/2 in our [0, 0x40000000) phase space.
    // Equivalently, sin_q24 input = fcOverSrQ31 (the upper 31 bits already
    // represent a half-cycle, so multiplying by 2 in the int domain is a
    // shift-left-by-1 then mask).
    uint32_t phase = fcOverSrQ31 << 1;  // 0..0xFFFFFFFF over 0..π
    i32 s = acid::dsp::sin_q24(phase);   // = sin(π * (fc/SR))
    return s << 1;  // 2 * sin(...)
}

// Linear interpolation between two Q24 values by t (also Q24, 0..ONE).
inline i32 lerp_q24(i32 a, i32 b, i32 t) {
    return a + mul_q24(b - a, t);
}

// cutoffParam (Q24, 0..ONE) → base cutoff in Hz Q24. Linear 80..4000 Hz.
inline i32 cutoff_param_to_hz_q24(i32 paramQ24) {
    constexpr i32 LO = 80 << Q24_SHIFT;
    constexpr i32 HI = 4000 << Q24_SHIFT;
    return lerp_q24(LO, HI, paramQ24);
}

// Map decayParam (Q24, 0..1) → tau in samples (Q24 SR ticks).
// tau_seconds = 0.05 + 1.45 * decayParam, tau_samples = tau_seconds * SR.
// Coefficient = exp(-1 / tau_samples). Caller pre-bakes this once.
inline i32 decay_param_to_coeff(i32 decayParamQ24) {
    constexpr i32 SR = AcbTb303Stage1::SAMPLE_RATE;
    // tau samples in Q24: tau_q24 = (0.05 + 1.45 * dp) * SR <<Q24
    // Compute tau samples in plain int (rounded), then -1/tau in Q24, then exp_q24.
    // For dp ∈ [0, 1] this maps to tau_samples ∈ [2205, 66150].
    constexpr int32_t TAU_LO_SAMPLES = 2205;       // 0.05 * 44100
    constexpr int32_t TAU_HI_SAMPLES = 66150;      // 1.50 * 44100
    int32_t tau = TAU_LO_SAMPLES +
                  ((static_cast<int64_t>(TAU_HI_SAMPLES - TAU_LO_SAMPLES) *
                    decayParamQ24) >> Q24_SHIFT);
    if (tau < 1) tau = 1;
    // exp(-1/tau). For tau >> 1 this is close to 1, want Q24 precision.
    // exp(-1/tau) = exp(x) with x = -1/tau, x ∈ [-0.00045, -1/1] ≈ [-1, -1.5e-5].
    // -1/tau in Q24: -(ONE_Q24) / tau, integer division.
    i32 xQ24 = -(ONE_Q24 / tau);
    return acid::dsp::exp_q24(xQ24);
}

}  // namespace

AcbTb303Stage1::AcbTb303Stage1() {
    // Default knobs: cutoff 0.4, reso 0.7, envMod 0.6, decay 0.4, accent 0.5.
    setCutoff   (static_cast<i32>(0.40 * ONE_Q24));
    setResonance(static_cast<i32>(0.70 * ONE_Q24));
    setEnvMod   (static_cast<i32>(0.60 * ONE_Q24));
    setDecay    (static_cast<i32>(0.40 * ONE_Q24));
    setAccentAmount(static_cast<i32>(0.50 * ONE_Q24));
}

void AcbTb303Stage1::setCutoff(i32 cutoffQ24) {
    if (cutoffQ24 < 0) cutoffQ24 = 0;
    if (cutoffQ24 > ONE_Q24) cutoffQ24 = ONE_Q24;
    m_cutoffParam = cutoffQ24;
}

void AcbTb303Stage1::setResonance(i32 resQ24) {
    if (resQ24 < 0) resQ24 = 0;
    if (resQ24 > ONE_Q24) resQ24 = ONE_Q24;
    // Q ∈ [1, 12] → 1/Q ∈ [1, 1/12]. lerp in Q24.
    constexpr i32 INV_Q_MIN = ONE_Q24;                     // 1/Q at Q=1
    constexpr i32 INV_Q_MAX = ONE_Q24 / 12;                // 1/Q at Q=12
    m_resonanceQc = lerp_q24(INV_Q_MIN, INV_Q_MAX, resQ24);
}

void AcbTb303Stage1::setEnvMod(i32 envModQ24) {
    if (envModQ24 < 0) envModQ24 = 0;
    if (envModQ24 > ONE_Q24) envModQ24 = ONE_Q24;
    m_envMod = envModQ24;
}

void AcbTb303Stage1::setDecay(i32 decayQ24) {
    if (decayQ24 < 0) decayQ24 = 0;
    if (decayQ24 > ONE_Q24) decayQ24 = ONE_Q24;
    i32 c = decay_param_to_coeff(decayQ24);
    m_envVcfCoeff = c;
    m_envVcaCoeff = c;
}

void AcbTb303Stage1::setAccentAmount(i32 accentQ24) {
    if (accentQ24 < 0) accentQ24 = 0;
    if (accentQ24 > ONE_Q24) accentQ24 = ONE_Q24;
    m_accentDepth = accentQ24;
}

void AcbTb303Stage1::noteOn(i32 noteHzQ24, bool slide, bool accent) {
    u32 newStep = hz_q24_to_step(noteHzQ24);

    if (slide && m_step != 0) {
        // 60 ms portamento. Per-sample linear ramp in step space.
        m_stepTarget = newStep;
        m_sliding = true;
        constexpr int RAMP_SAMPLES = SAMPLE_RATE * 60 / 1000;  // 2646
        int64_t diff = static_cast<int64_t>(newStep) - static_cast<int64_t>(m_step);
        m_stepRate = static_cast<u32>(diff / RAMP_SAMPLES);
    } else {
        m_step = newStep;
        m_stepTarget = newStep;
        m_sliding = false;
    }

    m_envVcf = ONE_Q24;
    m_envVca = ONE_Q24;
    m_envVcaTarget = ONE_Q24;  // gate held
    m_accent = accent;
}

void AcbTb303Stage1::noteOff() {
    m_envVcaTarget = 0;
}

i16 AcbTb303Stage1::tick() {
    // ---- slide ramp (linear in step space)
    if (m_sliding) {
        u32 newStep = m_step + m_stepRate;
        bool ascending = (m_stepRate & 0x80000000u) == 0;
        if (ascending ? (newStep >= m_stepTarget) : (newStep <= m_stepTarget)) {
            m_step = m_stepTarget;
            m_sliding = false;
        } else {
            m_step = newStep;
        }
    }

    // ---- DCO
    m_phase += m_step;
    i32 osc;
    if (m_wave == AcbWave::Saw) {
        // Map signed 32 phase to Q24 ∈ [-ONE, ONE).
        osc = static_cast<i32>(m_phase) >> 7;
    } else {
        osc = (m_phase & 0x80000000u) ? -ONE_Q24 : ONE_Q24;
    }

    // ---- VCF cutoff: base + envMod * envVcf * (accent boost in octaves)
    // base Hz (Q24)
    i32 fcQ24 = cutoff_param_to_hz_q24(m_cutoffParam);
    // env contribution in octaves: 3 * envMod * envVcf (* accent boost).
    // Q24 multiplication chains.
    i32 envOctaves = mul_q24(m_envMod, m_envVcf);  // 0..ONE in Q24
    // *3 octaves (= shift left by ~1.585 — approximate by *3 then /1)
    envOctaves = (envOctaves * 3);  // up to 3.0 in Q24
    if (m_accent) {
        // boost by 1 + 0.6 * accentDepth
        i32 boost = ONE_Q24 + mul_q24(static_cast<i32>(0.6 * ONE_Q24), m_accentDepth);
        envOctaves = mul_q24(envOctaves, boost);
    }
    // fc *= 2^envOctaves. Split into integer + fractional octaves.
    int wholeOct = envOctaves >> Q24_SHIFT;
    i32 fracOct  = envOctaves & (ONE_Q24 - 1);
    // 2^frac via pow2 LUT
    i32 fracMult = acid::dsp::pow2_unit_q24(fracOct);  // [ONE, 2*ONE]
    // fc *= fracMult; then shift by wholeOct.
    int64_t fc64 = (static_cast<int64_t>(fcQ24) * static_cast<int64_t>(fracMult)) >> Q24_SHIFT;
    if (wholeOct >= 0 && wholeOct < 16) fc64 <<= wholeOct;
    // Cap fc at 0.45 * SR to keep Chamberlin SVF stable.
    constexpr int64_t FC_MAX_Q24 =
        static_cast<int64_t>(0.45 * AcbTb303Stage1::SAMPLE_RATE) << Q24_SHIFT;
    if (fc64 > FC_MAX_Q24) fc64 = FC_MAX_Q24;
    if (fc64 < (1 << Q24_SHIFT)) fc64 = (1 << Q24_SHIFT);  // 1 Hz floor

    // fc / SR as Q31 phase fragment (so sin_q24 lookup maps π*x correctly).
    // Q31 norm = (fcQ24 / SR_int) << 7 (Q24 → Q31).
    uint32_t fcOverSrQ31 =
        static_cast<uint32_t>((fc64 / static_cast<int64_t>(SAMPLE_RATE)) << 7);
    i32 f = svf_f_from_norm(fcOverSrQ31);
    if (f > (3 << Q24_SHIFT) / 2) f = (3 << Q24_SHIFT) / 2;  // Chamberlin stability

    // Chamberlin SVF — tap "low".
    m_lpLow += mul_q24(f, m_lpBand);
    i32 high = osc - m_lpLow - mul_q24(m_resonanceQc, m_lpBand);
    m_lpBand += mul_q24(f, high);

    // ---- VCA
    i32 vcaGain = m_envVca;
    if (m_accent) {
        vcaGain += mul_q24(m_envVca, m_accentDepth);
        constexpr i32 GAIN_MAX = (3 * ONE_Q24) / 2;  // 1.5
        if (vcaGain > GAIN_MAX) vcaGain = GAIN_MAX;
    }
    i32 out = mul_q24(m_lpLow, vcaGain);

    // Advance envelopes.
    m_envVcf = mul_q24(m_envVcf, m_envVcfCoeff);
    if (m_envVcaTarget == 0) {
        m_envVca = mul_q24(m_envVca, m_envVcaCoeff);
    } else {
        m_envVca = m_envVcaTarget;
    }

    // Q24 → i16. Saw + Q resonance + accent → keep headroom: >> 12 shifts
    // Q24 unity (= 0x1000000) down to 4096, sat16 clamps the rest.
    i32 scaled = out >> (Q24_SHIFT - 12);
    return sat16(scaled);
}

}  // namespace acid::voices
