// cb_tbridge.hpp — TR-808-style claves / rimshot family.
//
// Topology: same T-bridge resonant tank as BD / Toms, but at much higher
// tuning (~2.5 kHz) and very short decay. The result is a dry "tock" with a
// thin tonal pitch — classic claves sound.
//
// Could in principle share TomTBridge with extended ranges, but at this
// f0 / decay region the impulse pre-multiplier and the Q range are different
// enough that it's clearer as its own class. Memory note says we may merge
// later once both are tuned by ear.

#pragma once
#include "fixed.hpp"

namespace acid::dsp {

class CBTBridge {
  public:
    explicit CBTBridge(int sampleRate);

    // tuning in [0, 1] — f0 ~1.5 kHz .. ~3.5 kHz
    // tone   in [0, 1] — Q (~6 .. ~20)
    // decay  in [0, 1] — env tau ~20 ms .. ~120 ms
    void setParams(float tuning, float tone, float decay);

    void trigger(float velocity);
    i16 tick();
    bool isActive() const { return m_env > 32; }

  private:
    int m_sampleRate;
    i32 m_low = 0, m_band = 0;
    i32 m_f = 0, m_q = 0;
    i32 m_env = 0, m_envCoeff = 0;
    i32 m_pendingImpulse = 0;
};

}  // namespace acid::dsp
