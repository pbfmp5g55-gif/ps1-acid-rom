// tom_tbridge.hpp — TR-808-style toms (LT / MT / HT).
//
// Same T-bridge resonator topology as BD, just at higher tuning. The real 808
// has three identical voice cards with different tuning capacitor values for
// LT (~80 Hz), MT (~140 Hz), HT (~200 Hz).
//
// We expose a single class with a free tuning parameter — instantiate three
// times in the engine with different tuning values.
//
// Difference from BD beyond tuning:
//   - Lower Q (~10) — tom has more body, less "boom"
//   - Shorter envelope range (~50 ms .. ~700 ms)
//   - Smaller pre-impulse multiplier (we sit at higher f0 so ringup amplitude
//     is naturally bigger per impulse).

#pragma once
#include "fixed.hpp"

namespace acid::dsp {

class TomTBridge {
  public:
    explicit TomTBridge(int sampleRate);

    // tuning in [0, 1] — f0 ~70 Hz .. ~250 Hz, covers LT/MT/HT range
    // tone   in [0, 1] — Q (~4 .. ~16)
    // decay  in [0, 1] — env tau ~50 ms .. ~700 ms
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
