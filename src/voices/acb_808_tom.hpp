// acb_808_tom.hpp — Integer Q24 TR-808 Tom voice. T-bridge resonator
// (higher f0, lower Q than BD) + exp env. Sample rate 44100 Hz.

#pragma once
#include "dsp/fixed.hpp"

namespace acid::voices {

class Acb808Tom {
  public:
    static constexpr int SAMPLE_RATE = 44100;

    Acb808Tom();

    void setTuning(acid::dsp::i32 tuningQ24);  // f0 70..250 Hz
    void setTone(acid::dsp::i32 toneQ24);      // Q 4..16
    void setDecay(acid::dsp::i32 decayQ24);    // tau 50..700 ms

    void trigger(acid::dsp::i32 velocityQ24);
    bool isActive() const { return m_env > 64; }

    acid::dsp::i16 tick();

  private:
    using i32 = acid::dsp::i32;

    i32 m_low = 0, m_band = 0;
    i32 m_f = 0, m_q = 0;
    i32 m_env = 0, m_envCoeff = 0;
    i32 m_pendingImpulse = 0;
};

}  // namespace acid::voices
