// acb_808_cp.hpp — Integer Q24 TR-808 CP (clap) voice.
// Two slightly detuned square waves summed → BPF → exp env, with the
// trigger producing a single velocity-shaped burst (the real 808 fires
// four rapid bursts; we collapse to one envelope for simplicity, the
// "BANG" character still survives).
//
// Sample rate 44100 Hz.

#pragma once
#include "dsp/fixed.hpp"

namespace acid::voices {

class Acb808Cp {
  public:
    static constexpr int SAMPLE_RATE = 44100;

    Acb808Cp();

    void setTuning(acid::dsp::i32 tuningQ24);  // f1 400..700 Hz, f2 = 1.48*f1
    void setDecay(acid::dsp::i32 decayQ24);    // tau 80..400 ms

    void trigger(acid::dsp::i32 velocityQ24);
    bool isActive() const { return m_env > 64; }

    acid::dsp::i16 tick();

  private:
    using i32 = acid::dsp::i32;
    using u32 = acid::dsp::u32;

    u32 m_phase1 = 0;
    u32 m_phase2 = 0x40000000u;  // slight initial offset
    u32 m_step1 = 0;
    u32 m_step2 = 0;

    // BPF on the sum.
    i32 m_bpLow = 0, m_bpBand = 0;
    i32 m_bpF = 0;
    i32 m_bpQ = 0;

    // Env + per-trigger velocity scaler.
    i32 m_env = 0, m_envCoeff = 0;
    i32 m_velocity = 0;
};

}  // namespace acid::voices
