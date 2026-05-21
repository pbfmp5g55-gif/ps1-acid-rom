// sd_twint.hpp — TR-808-style snare drum.
//
// Reference (community-published reverse engineering, e.g. Eric Archer's
// 808 walkthrough): SD is the sum of two sub-circuits.
//   1. "Tone" path: two parallel twin-T resonators (~185 Hz and ~330 Hz)
//      shock-excited by the trigger pulse — gives the drum its pitched body.
//   2. "Snappy" path: broadband noise → band-pass + steep envelope — gives
//      the buzz from the snare wires under the head.
// A single "snappy" pot mixes the two.
//
// Discrete-time model:
//   - Each twin-T → Chamberlin SVF (band-pass output). Two SVFs in parallel
//     summed at the output stage.
//   - Noise generator from noise.hpp, filtered through a third SVF (BPF
//     centered ~2 kHz to mimic the snare's high-freq character).
//   - Two amplitude envelopes — body (~150 ms) and snap (~80 ms).
//
// Not modeled: pre-emphasis around the snappy mix, output stage saturation.

#pragma once
#include "fixed.hpp"
#include "noise.hpp"

namespace acid::dsp {

class SDTwinT {
  public:
    explicit SDTwinT(int sampleRate);

    // tuning in [0, 1]    — body f0 (~150 Hz .. ~250 Hz)
    // snappy in [0, 1]    — mix between body (0) and snare buzz (1)
    // decay  in [0, 1]    — overall env time (~80 ms .. ~400 ms)
    void setParams(float tuning, float snappy, float decay);

    void trigger(float velocity);

    i16 tick();

    bool isActive() const { return m_envBody > 32 || m_envSnap > 32; }

  private:
    int m_sampleRate;

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

    // Trigger impulse pending for body resonators.
    i32 m_pendingImpulse = 0;

    Noise m_noise;
};

}  // namespace acid::dsp
