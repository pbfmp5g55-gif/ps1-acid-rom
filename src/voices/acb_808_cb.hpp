// acb_808_cb.hpp — TR-808 Cowbell. Same T-bridge topology as Tom, but
// pitched up (1.5–3.5 kHz) with shorter decay. Sample rate 44100 Hz.

#pragma once
#include "dsp/fixed.hpp"

namespace acid::voices {

class Acb808Cb {
  public:
    static constexpr int SAMPLE_RATE = 44100;
    Acb808Cb();

    void setTuning(acid::dsp::i32 tuningQ24);  // 1500..3500 Hz
    void setTone(acid::dsp::i32 toneQ24);      // Q 6..20
    void setDecay(acid::dsp::i32 decayQ24);    // tau 20..120 ms

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
