// acb_808_sd.hpp — Integer Q24 TR-808 SD voice for live render.
// Two parallel twin-T body resonators + noise→BPF "snappy" path, mixed
// via the snappy knob. Same topology as host's sd_twint, no cmath.

#pragma once
#include "dsp/fixed.hpp"
#include "dsp/noise.hpp"

namespace acid::voices {

class Acb808Sd {
  public:
    static constexpr int SAMPLE_RATE = 44100;

    Acb808Sd();

    // tuning Q24 0..1 → body f0 150..250 Hz (second body = 1.78 * f0)
    void setTuning(acid::dsp::i32 tuningQ24);
    // snappy Q24 0..1 → mix between body (0) and snare buzz (1)
    void setSnappy(acid::dsp::i32 snappyQ24);
    // decay Q24 0..1 → body env tau 80..400 ms (snap env = 0.5 × body)
    void setDecay(acid::dsp::i32 decayQ24);

    void trigger(acid::dsp::i32 velocityQ24);
    bool isActive() const { return m_envBody > 64 || m_envSnap > 64; }

    acid::dsp::i16 tick();

  private:
    using i32 = acid::dsp::i32;

    // Two body resonators (Chamberlin SVF state).
    i32 m_low1 = 0, m_band1 = 0;
    i32 m_low2 = 0, m_band2 = 0;
    i32 m_f1 = 0, m_f2 = 0;
    i32 m_qBody = 0;

    // Snappy BPF on noise.
    i32 m_lowS = 0, m_bandS = 0;
    i32 m_fS = 0;
    i32 m_qSnap = 0;

    // Envelopes.
    i32 m_envBody = 0, m_envBodyCoeff = 0;
    i32 m_envSnap = 0, m_envSnapCoeff = 0;

    // Mix.
    i32 m_bodyGain = 0;  // Q24, (1 - snappy)
    i32 m_snapGain = 0;  // Q24, snappy

    i32 m_pendingImpulse = 0;
    acid::dsp::Noise m_noise;
};

}  // namespace acid::voices
