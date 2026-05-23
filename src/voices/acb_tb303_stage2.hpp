// acb_tb303_stage2.hpp — Stilson-Smith naive Moog ladder integer voice.
// Same DCO/env/VCA layout as stage 1, but the VCF is a 4-pole cascade
// with one-sample-delayed feedback (k ≤ 2.5) instead of Chamberlin SVF.
//
// Sample rate locked to 44100 Hz.

#pragma once
#include "dsp/fixed.hpp"
#include "voices/acb_tb303_stage1.hpp"  // AcbWave enum reused

namespace acid::voices {

class AcbTb303Stage2 {
  public:
    static constexpr int SAMPLE_RATE = 44100;

    AcbTb303Stage2();

    void setWave(AcbWave w) { m_wave = w; }
    void setCutoff(acid::dsp::i32 cutoffQ24);
    void setResonance(acid::dsp::i32 resQ24);
    void setEnvMod(acid::dsp::i32 envModQ24);
    void setDecay(acid::dsp::i32 decayQ24);
    void setAccentAmount(acid::dsp::i32 accentQ24);

    void noteOn(int noteHz, bool slide, bool accent);
    void noteOff();
    bool isActive() const { return m_envVca > 0x4000; }

    acid::dsp::i16 tick();

  private:
    using i32 = acid::dsp::i32;
    using u32 = acid::dsp::u32;

    AcbWave m_wave = AcbWave::Saw;

    // DCO
    u32 m_phase     = 0;
    u32 m_step      = 0;
    u32 m_stepTarget= 0;
    u32 m_stepRate  = 0;
    bool m_sliding  = false;

    // Ladder filter (4-pole cascade, Stilson naive Moog).
    i32 m_y1 = 0, m_y2 = 0, m_y3 = 0, m_y4 = 0;

    // Pre-baked params.
    i32 m_cutoffParam = 0;
    i32 m_resonance   = 0;  // ladder feedback k (Q24, capped at 2.5)
    i32 m_envMod      = 0;
    i32 m_envVcfCoeff = 0;
    i32 m_envVcaCoeff = 0;
    i32 m_accentDepth = 0;

    // Envs.
    i32 m_envVcf       = 0;
    i32 m_envVca       = 0;
    i32 m_envVcaTarget = 0;

    bool m_accent = false;
};

}  // namespace acid::voices
