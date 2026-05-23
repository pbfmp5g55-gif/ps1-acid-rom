// acb_808_bd.hpp — Integer Q24 version of the TR-808 BD voice for PS1
// live render. Same T-bridge resonator + exp env layout as host's
// src/dsp/bd_tbridge, but every transcendental goes through qmath LUTs.
//
// Sample rate locked to 44100 Hz so coefficients can be pre-baked at
// param-set time.

#pragma once
#include "dsp/fixed.hpp"

namespace acid::voices {

class Acb808Bd {
  public:
    static constexpr int SAMPLE_RATE = 44100;

    Acb808Bd();

    // tuning Q24 0..1 → f0 38..85 Hz
    void setTuning(acid::dsp::i32 tuningQ24);
    // tone Q24 0..1 → Q 2..40 (high tone = high Q = longer ring)
    void setTone(acid::dsp::i32 toneQ24);
    // decay Q24 0..1 → env tau 40..1500 ms
    void setDecay(acid::dsp::i32 decayQ24);

    // Trigger a hit. velocity Q24 in [0, 1] (default 1.0 = full hit).
    void trigger(acid::dsp::i32 velocityQ24);
    bool isActive() const { return m_env > 64; }

    acid::dsp::i16 tick();

  private:
    using i32 = acid::dsp::i32;

    // Chamberlin SVF tank (band-pass output is the resonator we listen to).
    i32 m_low  = 0;
    i32 m_band = 0;
    i32 m_f = 0;        // SVF f coefficient (Q24)
    i32 m_q = 0;        // 1/Q (Q24)

    // Amplitude env (post-resonator).
    i32 m_env       = 0;
    i32 m_envCoeff  = 0;

    // Trigger impulse for next tick.
    i32 m_pendingImpulse = 0;
};

}  // namespace acid::voices
