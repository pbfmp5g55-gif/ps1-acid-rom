// cp_burst.hpp — TR-808-style cowbell.
//
// Reference (community RE): cowbell = two square oscillators at ~540 Hz and
// ~800 Hz (close to a perfect fourth/fifth interval), summed and run through
// a moderate band-pass with a snappy decay envelope. The interval between
// the two squares produces the cowbell's characteristic open "clank".
//
// Discrete model:
//   - Two phase accumulators (same square trick as HH/CY)
//   - BPF: Chamberlin SVF, band-pass output, center ~900 Hz
//   - Env: single exp decay (~150 ms)

#pragma once
#include "fixed.hpp"

namespace acid::dsp {

class CPBurst {
  public:
    explicit CPBurst(int sampleRate);

    // tuning in [0, 1] — base freq (~400 Hz .. ~700 Hz)
    // decay  in [0, 1] — env tau ~80 ms .. ~400 ms
    void setParams(float tuning, float decay);

    void trigger(float velocity);
    i16 tick();
    bool isActive() const { return m_env > 32; }

  private:
    int m_sampleRate;

    u32 m_phase1 = 0, m_phase2 = 0;
    u32 m_step1 = 0, m_step2 = 0;

    i32 m_bpLow = 0, m_bpBand = 0;
    i32 m_bpF = 0, m_bpQ = 0;

    i32 m_env = 0, m_envCoeff = 0;
    i32 m_velocity = 0;
};

}  // namespace acid::dsp
