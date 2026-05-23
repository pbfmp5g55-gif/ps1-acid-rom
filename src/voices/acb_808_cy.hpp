// acb_808_cy.hpp — TR-808 Cymbal. Same six-squares + dual SVF DSP as HH
// but a different non-harmonic freq set (so HH and CY don't sound
// identical when played together) and a much longer envelope.
// 480/565/678/820/977/1115 Hz family.

#pragma once
#include "dsp/fixed.hpp"

namespace acid::voices {

class Acb808Cy {
  public:
    static constexpr int SAMPLE_RATE = 44100;
    Acb808Cy();

    void setDecay(acid::dsp::i32 decayQ24);         // tau 800..4000 ms
    void setBrightness(acid::dsp::i32 brightQ24);
    void setTune(acid::dsp::i32 tuneQ24);

    void trigger(acid::dsp::i32 velocityQ24);
    bool isActive() const { return m_env > 64; }

    acid::dsp::i16 tick();

  private:
    using i32 = acid::dsp::i32;
    using u32 = acid::dsp::u32;

    u32 m_phase[6] = {0};
    u32 m_step[6]  = {0};

    i32 m_hp_low = 0, m_hp_band = 0;
    i32 m_hp_f = 0, m_hp_q = 0;
    i32 m_bp_low = 0, m_bp_band = 0;
    i32 m_bp_f = 0, m_bp_q = 0;

    i32 m_env = 0, m_envCoeff = 0;
    i32 m_velocity = 0;
};

}  // namespace acid::voices
