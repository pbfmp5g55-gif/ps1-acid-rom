// acb_tb303_stage2.cpp — see acb_tb303_stage2.hpp.

#include "voices/acb_tb303_stage2.hpp"
#include "dsp/qmath.hpp"

namespace acid::voices {

using acid::dsp::mul_q24;
using acid::dsp::sat16;
using acid::dsp::Q24_SHIFT;
constexpr acid::dsp::i32 ONE_Q24 = 1 << Q24_SHIFT;

namespace {

constexpr uint32_t STEP_PER_HZ = 97392;
constexpr acid::dsp::i32 TWO_PI_OVER_SR_Q24 = 2390;

inline acid::dsp::u32 hz_to_step(int hz) {
    if (hz < 1) hz = 1;
    return static_cast<acid::dsp::u32>(hz) * STEP_PER_HZ;
}

inline acid::dsp::i32 lerp_q24(acid::dsp::i32 a, acid::dsp::i32 b, acid::dsp::i32 t) {
    return a + mul_q24(b - a, t);
}

inline acid::dsp::i32 decay_param_to_coeff(acid::dsp::i32 decayParamQ24) {
    constexpr int32_t TAU_LO_SAMPLES = 2205;
    constexpr int32_t TAU_HI_SAMPLES = 66150;
    int32_t tau = TAU_LO_SAMPLES +
                  static_cast<int32_t>(
                      (static_cast<int64_t>(TAU_HI_SAMPLES - TAU_LO_SAMPLES) *
                       decayParamQ24) >> Q24_SHIFT);
    if (tau < 1) tau = 1;
    return acid::dsp::exp_q24(-(ONE_Q24 / tau));
}

}  // namespace

AcbTb303Stage2::AcbTb303Stage2() {
    setCutoff   (static_cast<i32>(0.40 * ONE_Q24));
    setResonance(static_cast<i32>(0.70 * ONE_Q24));
    setEnvMod   (static_cast<i32>(0.60 * ONE_Q24));
    setDecay    (static_cast<i32>(0.40 * ONE_Q24));
    setAccentAmount(static_cast<i32>(0.50 * ONE_Q24));
}

void AcbTb303Stage2::setCutoff(i32 cutoffQ24) {
    if (cutoffQ24 < 0) cutoffQ24 = 0;
    if (cutoffQ24 > ONE_Q24) cutoffQ24 = ONE_Q24;
    m_cutoffParam = cutoffQ24;
}

void AcbTb303Stage2::setResonance(i32 resQ24) {
    if (resQ24 < 0) resQ24 = 0;
    if (resQ24 > ONE_Q24) resQ24 = ONE_Q24;
    // k ∈ [0, 2.5] — stage 2 caps below the naive Moog stability cliff.
    constexpr i32 K_MAX = (5 * ONE_Q24) / 2;  // 2.5
    m_resonance = lerp_q24(0, K_MAX, resQ24);
}

void AcbTb303Stage2::setEnvMod(i32 envModQ24) {
    if (envModQ24 < 0) envModQ24 = 0;
    if (envModQ24 > ONE_Q24) envModQ24 = ONE_Q24;
    m_envMod = envModQ24;
}

void AcbTb303Stage2::setDecay(i32 decayQ24) {
    if (decayQ24 < 0) decayQ24 = 0;
    if (decayQ24 > ONE_Q24) decayQ24 = ONE_Q24;
    i32 c = decay_param_to_coeff(decayQ24);
    m_envVcfCoeff = c;
    m_envVcaCoeff = c;
}

void AcbTb303Stage2::setAccentAmount(i32 accentQ24) {
    if (accentQ24 < 0) accentQ24 = 0;
    if (accentQ24 > ONE_Q24) accentQ24 = ONE_Q24;
    m_accentDepth = accentQ24;
}

void AcbTb303Stage2::noteOn(int noteHz, bool slide, bool accent) {
    u32 newStep = hz_to_step(noteHz);
    if (slide && m_step != 0) {
        m_stepTarget = newStep;
        m_sliding = true;
        constexpr int RAMP_SAMPLES = SAMPLE_RATE * 60 / 1000;
        i32 diff = static_cast<i32>(newStep - m_step);
        m_stepRate = static_cast<u32>(diff / RAMP_SAMPLES);
    } else {
        m_step = newStep;
        m_stepTarget = newStep;
        m_sliding = false;
    }
    m_envVcf = ONE_Q24;
    m_envVca = ONE_Q24;
    m_envVcaTarget = ONE_Q24;
    m_accent = accent;
}

void AcbTb303Stage2::noteOff() { m_envVcaTarget = 0; }

acid::dsp::i16 AcbTb303Stage2::tick() {
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

    // DCO
    m_phase += m_step;
    i32 osc;
    if (m_wave == AcbWave::Saw) {
        osc = static_cast<i32>(m_phase) >> 7;
    } else {
        osc = (m_phase & 0x80000000u) ? -ONE_Q24 : ONE_Q24;
    }

    // VCF cutoff (Hz int, after env mod + accent boost).
    i32 fc_base_hz = 80 + (mul_q24(3920 << Q24_SHIFT, m_cutoffParam) >> Q24_SHIFT);
    i32 envOctaves = mul_q24(m_envMod, m_envVcf) * 3;
    if (m_accent) {
        i32 boost = ONE_Q24 + mul_q24(static_cast<i32>(0.6 * ONE_Q24), m_accentDepth);
        envOctaves = mul_q24(envOctaves, boost);
    }
    int wholeOct = envOctaves >> Q24_SHIFT;
    i32 fracOct  = envOctaves & (ONE_Q24 - 1);
    i32 fracMult = acid::dsp::pow2_unit_q24(fracOct);
    i32 fc_hz = static_cast<i32>(
        (static_cast<int64_t>(fc_base_hz) * fracMult) >> Q24_SHIFT);
    if (wholeOct > 0 && wholeOct < 16) fc_hz <<= wholeOct;
    constexpr i32 FC_MAX_HZ = (SAMPLE_RATE * 45) / 100;
    if (fc_hz > FC_MAX_HZ) fc_hz = FC_MAX_HZ;
    if (fc_hz < 1)         fc_hz = 1;
    i32 g = fc_hz * TWO_PI_OVER_SR_Q24;  // ~2π fc / SR (Q24, small-angle)
    if (g > (99 * ONE_Q24) / 100) g = (99 * ONE_Q24) / 100;

    // Ladder cascade with capped feedback.
    i32 fb = mul_q24(m_resonance, m_y4);
    constexpr i32 FB_CAP = 3 << Q24_SHIFT;
    if (fb >  FB_CAP) fb =  FB_CAP;
    if (fb < -FB_CAP) fb = -FB_CAP;
    i32 u = osc - fb;
    m_y1 += mul_q24(g, u      - m_y1);
    m_y2 += mul_q24(g, m_y1   - m_y2);
    m_y3 += mul_q24(g, m_y2   - m_y3);
    m_y4 += mul_q24(g, m_y3   - m_y4);

    // VCA
    i32 vcaGain = m_envVca;
    if (m_accent) {
        vcaGain += mul_q24(m_envVca, m_accentDepth);
        constexpr i32 GAIN_MAX = (3 * ONE_Q24) / 2;
        if (vcaGain > GAIN_MAX) vcaGain = GAIN_MAX;
    }
    i32 out = mul_q24(m_y4, vcaGain);

    m_envVcf = mul_q24(m_envVcf, m_envVcfCoeff);
    if (m_envVcaTarget == 0) {
        m_envVca = mul_q24(m_envVca, m_envVcaCoeff);
    } else {
        m_envVca = m_envVcaTarget;
    }

    i32 scaled = out >> (Q24_SHIFT - 11);
    return sat16(scaled);
}

}  // namespace acid::voices
