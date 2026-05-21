// tb303_stage2.hpp — TB-303-style acid bass, stage 2.
//
// What stage 2 adds over stage 1:
//   The real 303 VCF is a Sallen-Key cascade with diode clippers in the
//   feedback path — perceptually a 24 dB/oct resonant lowpass that can
//   self-oscillate. Stage 1 used a 1-pole Chamberlin SVF as a placeholder so
//   we could verify the rest of the voice (DCO / env / accent / slide /
//   trigger sequencing). Stage 2 replaces only the filter with a 4-pole
//   Moog-ladder approximation (Stilson-Smith / naive ZDF cascade), which is
//   what gives the 303 its squelchy bite.
//
// Filter math (per sample, in Q24):
//   g = 2*pi*fc/fs   (small-cutoff approximation; sufficient at SR=22050
//                    for fc < ~3 kHz which covers env-modulated 303 range)
//   feedback = res * y4_prev_sample
//   u' = in - feedback
//   y1 = y1 + g*(u' - y1)
//   y2 = y2 + g*(y1 - y2)
//   y3 = y3 + g*(y2 - y3)
//   y4 = y4 + g*(y3 - y4)
//   out = y4
// The 4-cascade gives -90° phase per stage = 360° total at fc, so positive
// feedback there self-oscillates at res = 4. We cap res at 3.8 to leave a
// margin against fixed-point quantisation pushing it into instability.
//
// DCO / envs / accent / slide are reimplemented inline here (copied from
// stage 1) so M6's stage-2 voice is one self-contained class. They will get
// hoisted into a shared base once M9 (PSVJ integration) adds a third
// derivative (e.g. for the VJ-driven generative variant).

#pragma once
#include "fixed.hpp"

namespace acid::dsp {

enum class TB303Stage2Wave : int { Saw = 0, Square = 1 };

class TB303Stage2 {
  public:
    explicit TB303Stage2(int sampleRate);

    void setParams(TB303Stage2Wave w, float cutoff, float resonance,
                   float envMod, float decay, float accentAmount);

    void noteOn(float noteHz, bool slide, bool accent);
    void noteOff();
    i16 tick();
    bool isActive() const { return m_envVca > 16; }

  private:
    int m_sampleRate;

    // DCO state.
    u32 m_phase = 0, m_step = 0, m_stepTarget = 0, m_stepRate = 0;
    bool m_sliding = false;
    TB303Stage2Wave m_wave = TB303Stage2Wave::Saw;

    // VCF: 4-pole Moog ladder cascade.
    i32 m_y1 = 0, m_y2 = 0, m_y3 = 0, m_y4 = 0;
    i32 m_resonance = 0;     // Q24, the 'k' factor (cap ~3.8)
    i32 m_envMod = 0;
    float m_cutoffParam = 0.5f;

    i32 m_envVcf = 0, m_envVcfCoeff = 0;
    i32 m_envVca = 0, m_envVcaCoeff = 0;
    i32 m_envVcaTarget = 0;

    bool m_accent = false;
    i32 m_accentDepth = 0;
};

}  // namespace acid::dsp
