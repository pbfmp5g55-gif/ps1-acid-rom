// tb303_stage2.cpp — see tb303_stage2.hpp.

#include "dsp/tb303_stage2.hpp"
#include <cmath>

namespace acid::dsp {

namespace {
float lerp(float lo, float hi, float t) { return lo + (hi - lo) * t; }
float clamp01(float t) { return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t); }

u32 freq_to_step(int sampleRate, float f) {
    float fraction = f / static_cast<float>(sampleRate);
    if (fraction < 0.0f) fraction = 0.0f;
    if (fraction > 0.49f) fraction = 0.49f;
    return static_cast<u32>(fraction * 4294967296.0);
}

constexpr float TWO_PI = 6.28318530f;
}  // namespace

TB303Stage2::TB303Stage2(int sampleRate) : m_sampleRate(sampleRate) {
    setParams(TB303Stage2Wave::Saw, 0.4f, 0.7f, 0.6f, 0.4f, 0.5f);
}

void TB303Stage2::setParams(TB303Stage2Wave w, float cutoff, float resonance,
                            float envMod, float decay, float accentAmount) {
    m_wave = w;
    m_cutoffParam = clamp01(cutoff);

    // Resonance maps to ladder feedback. The naive Moog cascade we use
    // (one-sample-delayed feedback) loses stability margin as g grows, so
    // we cap k at 2.5 in stage 2 — still squelchy, no need to chase 4.0.
    // Stage 3 (future, with proper Stilson g-compensation or ZDF root
    // solving) can push k higher.
    float k = lerp(0.0f, 2.5f, clamp01(resonance));
    m_resonance = to_q24(k);

    m_envMod = to_q24(clamp01(envMod));

    float tau = lerp(0.05f, 1.5f, clamp01(decay));
    i32 coeff = to_q24(std::exp(-1.0f / (static_cast<float>(m_sampleRate) * tau)));
    m_envVcfCoeff = coeff;
    m_envVcaCoeff = coeff;

    m_accentDepth = to_q24(clamp01(accentAmount));
}

void TB303Stage2::noteOn(float noteHz, bool slide, bool accent) {
    u32 newStep = freq_to_step(m_sampleRate, noteHz);
    if (slide && m_step != 0) {
        m_stepTarget = newStep;
        m_sliding = true;
        int rampSamples = m_sampleRate * 60 / 1000;
        if (rampSamples < 1) rampSamples = 1;
        i64 diff = static_cast<i64>(newStep) - static_cast<i64>(m_step);
        m_stepRate = static_cast<u32>(diff / rampSamples);
    } else {
        m_step = newStep;
        m_stepTarget = newStep;
        m_sliding = false;
    }
    m_envVcf = to_q24(1.0f);
    m_envVca = to_q24(1.0f);
    m_envVcaTarget = to_q24(1.0f);
    m_accent = accent;
}

void TB303Stage2::noteOff() { m_envVcaTarget = 0; }

i16 TB303Stage2::tick() {
    // slide ramp
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
    if (m_wave == TB303Stage2Wave::Saw) {
        osc = static_cast<i32>(m_phase) >> 7;
    } else {
        constexpr i32 SQUARE_HI = 1 << Q24_SHIFT;
        osc = (m_phase & 0x80000000u) ? -SQUARE_HI : SQUARE_HI;
    }

    // VCF cutoff = base + envMod * envVcf * (accent boost)
    float baseHz = lerp(80.0f, 4000.0f, m_cutoffParam);
    float envOct = 3.0f * q24_to_f(m_envMod) * q24_to_f(m_envVcf);
    if (m_accent) envOct *= 1.0f + 0.6f * q24_to_f(m_accentDepth);
    float fc = baseHz * std::pow(2.0f, envOct);
    if (fc > m_sampleRate * 0.45f) fc = m_sampleRate * 0.45f;

    // g = 2*pi*fc/fs (small-angle good enough for fc < SR/3 ≈ 7 kHz)
    float gf = TWO_PI * fc / static_cast<float>(m_sampleRate);
    if (gf > 0.99f) gf = 0.99f;
    i32 g = to_q24(gf);

    // Ladder cascade with one-sample-delayed feedback (Stilson "naive Moog"
    // — simplest stable form, frequency response slightly off vs proper ZDF
    // at high fc but musically close in the 303's working range).
    //
    // Soft-clamp the feedback magnitude so a transient that runs the ladder
    // into resonance can't pin the SVF outputs at ±large values for many
    // samples (which would otherwise saturate the i16 path downstream and
    // mask the velocity / accent differences in tests).
    i32 fb = mul_q24(m_resonance, m_y4);
    constexpr i32 FB_CAP = (3 << Q24_SHIFT);  // ±3.0 Q24
    if (fb >  FB_CAP) fb =  FB_CAP;
    if (fb < -FB_CAP) fb = -FB_CAP;
    i32 u  = osc - fb;
    m_y1 += mul_q24(g, u - m_y1);
    m_y2 += mul_q24(g, m_y1 - m_y2);
    m_y3 += mul_q24(g, m_y2 - m_y3);
    m_y4 += mul_q24(g, m_y3 - m_y4);

    // VCA
    i32 vcaGain = m_envVca;
    if (m_accent) {
        vcaGain += mul_q24(m_envVca, m_accentDepth);
        if (vcaGain > to_q24(1.5f)) vcaGain = to_q24(1.5f);
    }
    i32 out = mul_q24(m_y4, vcaGain);

    m_envVcf = mul_q24(m_envVcf, m_envVcfCoeff);
    if (m_envVcaTarget == 0) {
        m_envVca = mul_q24(m_envVca, m_envVcaCoeff);
    } else {
        m_envVca = m_envVcaTarget;
    }

    // 4-pole cascade + capped feedback keeps output bounded ~Q24 ±2 worst
    // case. >> (Q24_SHIFT - 11) = Q24 unity → 2048 i16, leaves headroom.
    i32 scaled = out >> (Q24_SHIFT - 11);
    return sat16(scaled);
}

}  // namespace acid::dsp
