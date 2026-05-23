// acb_909_sd.hpp — TR-909 SD voice (two-band twin-T body + narrow snap BPF).

#pragma once
#include "dsp/fixed.hpp"
#include "dsp/noise.hpp"

namespace acid::voices {

class Acb909Sd {
  public:
    static constexpr int SAMPLE_RATE = 44100;
    Acb909Sd();

    void setTuning(acid::dsp::i32 tuningQ24);  // f1 250..400, f2 = 3.2*f1
    void setSnappy(acid::dsp::i32 snappyQ24);
    void setDecay(acid::dsp::i32 decayQ24);

    void trigger(acid::dsp::i32 velocityQ24);
    bool isActive() const { return m_envBody > 64 || m_envSnap > 64; }

    acid::dsp::i16 tick();

  private:
    using i32 = acid::dsp::i32;

    // Two body resonators.
    i32 m_low1 = 0, m_band1 = 0;
    i32 m_low2 = 0, m_band2 = 0;
    i32 m_f1 = 0, m_f2 = 0;
    i32 m_qBody = 0;

    // Snap BPF on noise.
    i32 m_lowS = 0, m_bandS = 0;
    i32 m_fS = 0;
    i32 m_qSnap = 0;

    i32 m_envBody = 0, m_envBodyCoeff = 0;
    i32 m_envSnap = 0, m_envSnapCoeff = 0;

    i32 m_bodyGain = 0;
    i32 m_snapGain = 0;

    i32 m_pendingImpulse = 0;
    acid::dsp::Noise m_noise;
};

}  // namespace acid::voices
