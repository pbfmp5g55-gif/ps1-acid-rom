// tb303_stage1.cpp — see tb303_stage1.hpp.

#include "dsp/tb303_stage1.hpp"
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

i32 svf_f(int sampleRate, float fc) {
    float arg = 3.14159265f * fc / static_cast<float>(sampleRate);
    if (arg > 1.0f) arg = 1.0f;  // tame Chamberlin instability margin
    return to_q24(2.0f * std::sin(arg));
}
}  // namespace

TB303Stage1::TB303Stage1(int sampleRate) : m_sampleRate(sampleRate) {
    setParams(TB303Wave::Saw, 0.4f, 0.7f, 0.6f, 0.4f, 0.5f);
}

void TB303Stage1::setParams(TB303Wave w, float cutoff, float resonance,
                            float envMod, float decay, float accentAmount) {
    m_wave = w;
    m_cutoffParam = clamp01(cutoff);

    // Resonance: in real 303, Q goes from ~1 (no peak) to ~12 (self-oscillating
    // edge). Mapped to Chamberlin q_coeff = 1/Q.
    float Q = lerp(1.0f, 12.0f, clamp01(resonance));
    m_resonanceQc = to_q24(1.0f / Q);

    // Env mod is stored as a Q24 multiplier for the env's contribution to
    // cutoff. At envMod=1.0 the env can push cutoff up by ~3 octaves at
    // full env, which is what gives the 303 its quack.
    m_envMod = to_q24(clamp01(envMod));

    // Env decays — both envelopes share the same time constant in stage 1.
    // (Real 303 has slightly different VCF/VCA env shapes; revisit in M6.)
    float tau = lerp(0.05f, 1.5f, clamp01(decay));
    i32 coeff = to_q24(std::exp(-1.0f / (static_cast<float>(m_sampleRate) * tau)));
    m_envVcfCoeff = coeff;
    m_envVcaCoeff = coeff;

    m_accentDepth = to_q24(clamp01(accentAmount));
}

void TB303Stage1::noteOn(float noteHz, bool slide, bool accent) {
    u32 newStep = freq_to_step(m_sampleRate, noteHz);

    if (slide && m_step != 0) {
        // 60 ms portamento. Pre-compute per-sample step delta as a signed
        // integer; we ramp linearly in step space (close enough to log-pitch
        // for a glide that short).
        m_stepTarget = newStep;
        m_sliding = true;
        int rampSamples = m_sampleRate * 60 / 1000;
        if (rampSamples < 1) rampSamples = 1;
        i64 diff = static_cast<i64>(newStep) - static_cast<i64>(m_step);
        m_stepRate = static_cast<u32>(diff / rampSamples);  // wraps as signed
    } else {
        m_step = newStep;
        m_stepTarget = newStep;
        m_sliding = false;
    }

    m_envVcf = to_q24(1.0f);
    m_envVca = to_q24(1.0f);
    m_envVcaTarget = to_q24(1.0f);  // gate high
    m_accent = accent;
}

void TB303Stage1::noteOff() {
    m_envVcaTarget = 0;  // gate low — env decays without re-trigger
}

i16 TB303Stage1::tick() {
    // ---- slide ramp (linear in step space)
    if (m_sliding) {
        // step += stepRate, until we cross the target
        u32 newStep = m_step + m_stepRate;
        // crossed?
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
    if (m_wave == TB303Wave::Saw) {
        // Saw: phase as signed 32 → Q24-ish in [-1, 1).
        osc = static_cast<i32>(m_phase) >> 7;  // top 25 bits, ≈ Q24
    } else {
        // Square.
        constexpr i32 SQUARE_HI = 1 << Q24_SHIFT;
        osc = (m_phase & 0x80000000u) ? -SQUARE_HI : SQUARE_HI;
    }

    // ---- VCF cutoff: base + envMod * envVcf * (accent boost)
    // Map cutoffParam → base Hz in [80, 4000] log-ish (we just linear in
    // Hz for stage 1 — the env mod is what gives the perceived motion).
    float baseHz = lerp(80.0f, 4000.0f, m_cutoffParam);
    // env contribution in octaves: up to 3 octaves when envMod=1.0
    float envOctaves = 3.0f * q24_to_f(m_envMod) * q24_to_f(m_envVcf);
    if (m_accent) {
        envOctaves *= 1.0f + 0.6f * q24_to_f(m_accentDepth);
    }
    float fc = baseHz * std::pow(2.0f, envOctaves);
    // Cap fc below Nyquist for Chamberlin stability.
    if (fc > m_sampleRate * 0.45f) fc = m_sampleRate * 0.45f;
    i32 f = svf_f(m_sampleRate, fc);

    // Chamberlin SVF — we tap the "low" output.
    m_lpLow += mul_q24(f, m_lpBand);
    i32 high = osc - m_lpLow - mul_q24(m_resonanceQc, m_lpBand);
    m_lpBand += mul_q24(f, high);

    // ---- VCA
    i32 vcaGain = m_envVca;
    if (m_accent) {
        // boost: vca += accentDepth * env
        vcaGain += mul_q24(m_envVca, m_accentDepth);
        if (vcaGain > to_q24(1.5f)) vcaGain = to_q24(1.5f);
    }
    i32 out = mul_q24(m_lpLow, vcaGain);

    // Advance envelopes. VCA env tracks target (1.0 while gated, 0 after
    // noteOff) with the same time constant on the way down as the VCF env.
    m_envVcf = mul_q24(m_envVcf, m_envVcfCoeff);
    if (m_envVcaTarget == 0) {
        m_envVca = mul_q24(m_envVca, m_envVcaCoeff);
    } else {
        // Gate held — VCF env still decays, but VCA stays at 1 until released.
        m_envVca = m_envVcaTarget;
    }

    // Q24 → i16. Saw is ±1 Q24, resonant LPF can amplify ~Q× at cutoff,
    // accent multiplies by another 1.5×. >> 12 (= Q24 unity → 4096) leaves
    // enough headroom for Q=12 + accent + env without clipping the i16.
    i32 scaled = out >> (Q24_SHIFT - 12);
    return sat16(scaled);
}

}  // namespace acid::dsp
