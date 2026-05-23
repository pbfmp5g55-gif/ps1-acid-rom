// acb_909_bd.hpp — TR-909 BD voice (T-bridge body + noise click stage).

#pragma once
#include "dsp/fixed.hpp"
#include "dsp/noise.hpp"

namespace acid::voices {

class Acb909Bd {
  public:
    static constexpr int SAMPLE_RATE = 44100;
    Acb909Bd();

    void setTuning(acid::dsp::i32 tuningQ24);  // 55..110 Hz
    void setTone(acid::dsp::i32 toneQ24);      // Q 4..30
    void setDecay(acid::dsp::i32 decayQ24);    // tau 80..1200 ms
    void setAttack(acid::dsp::i32 attackQ24);  // click amount 0..0.6

    void trigger(acid::dsp::i32 velocityQ24);
    bool isActive() const { return m_env > 64 || m_clickEnv > 64; }

    acid::dsp::i16 tick();

  private:
    using i32 = acid::dsp::i32;

    i32 m_low = 0, m_band = 0;
    i32 m_f = 0, m_q = 0;
    i32 m_env = 0, m_envCoeff = 0;
    i32 m_clickEnv = 0, m_clickEnvCoeff = 0;
    i32 m_clickGain = 0;
    i32 m_pendingImpulse = 0;
    acid::dsp::Noise m_noise;
};

}  // namespace acid::voices
