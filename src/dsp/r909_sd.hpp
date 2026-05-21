// r909_sd.hpp — TR-909-style snare drum.
//
// 909 SD body is a pair of analog twin-T resonators (~340 Hz and ~1.1 kHz),
// and the snare "snap" is a stored PCM sample played back in parallel — that
// hybrid analog-body + digital-snap design is why the 909 SD sounds crisper
// than the all-analog 808 SD. We approximate the snap as a short noise burst
// through a high-Q BPF, which gets close to the perceptual result without
// needing a baked sample.
//
// Reuses the same Chamberlin SVF approach the rest of the 808 family uses;
// stable here because the highest BPF center (~3.5 kHz) keeps f + 1/Q under
// the 2.0 instability bound.

#pragma once
#include "fixed.hpp"
#include "noise.hpp"

namespace acid::dsp {

class R909SD {
  public:
    explicit R909SD(int sampleRate);

    // tuning in [0, 1]    — body f0 lower band (~250 Hz .. ~400 Hz)
    // snappy in [0, 1]    — body vs snare buzz mix
    // decay  in [0, 1]    — overall env tau (~100 ms .. ~350 ms)
    void setParams(float tuning, float snappy, float decay);

    void trigger(float velocity);
    i16 tick();
    bool isActive() const { return m_envBody > 32 || m_envSnap > 32; }

  private:
    int m_sampleRate;

    // Two body resonators.
    i32 m_low1 = 0, m_band1 = 0;
    i32 m_low2 = 0, m_band2 = 0;
    i32 m_f1 = 0, m_f2 = 0;
    i32 m_qBody = 0;

    // Snap BPF on noise — higher center, higher Q than 808 SD.
    i32 m_lowS = 0, m_bandS = 0;
    i32 m_fS = 0, m_qSnap = 0;

    i32 m_envBody = 0, m_envBodyCoeff = 0;
    i32 m_envSnap = 0, m_envSnapCoeff = 0;
    i32 m_bodyGain = 0, m_snapGain = 0;

    i32 m_pendingImpulse = 0;
    Noise m_noise;
};

}  // namespace acid::dsp
