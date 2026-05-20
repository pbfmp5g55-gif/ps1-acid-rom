// bd_tbridge.hpp — TR-808-style bass drum, derived from the analog circuit.
//
// Reference circuit (community-published reverse engineering, e.g. Eric
// Archer's TR-808 schematic walkthrough): the BD voice is a resonant T-bridge
// oscillator (Q14 / Q15 dual op-amps with positive feedback through a tank
// network) that is shock-excited by a short trigger pulse, then rings down
// freely. The "tone" knob raises the tank's Q (sustains longer), the "decay"
// knob lengthens an envelope that amplitude-modulates the ringdown.
//
// Topology → discrete-time model:
//   - The T-bridge tank is a 2nd-order resonant section. We use a state-
//     variable IIR (Chamberlin form) with feedback q and frequency f0,
//     because it maps cleanly onto the analog topology (the two integrators
//     mirror the two energy-storage elements in the T-bridge).
//   - The trigger pulse is a single-sample impulse scaled by velocity. In
//     the real circuit this is a fast spike from the 1µF coupling cap; in
//     discrete time the equivalent excitation is delta-shaped.
//   - The amplitude envelope is a simple R/C exponential decay — the same
//     1-pole as the analog C charge-discharge through the decay-pot R.
//
// We do NOT model:
//   - The op-amp slew limit (negligible in the audible range here)
//   - Power-rail compression (a separate "punch" mod, deferred)
//   - The output emitter follower's small ~6dB shelf (cosmetic)
//
// Output sample rate is configurable; the SPU side currently targets 22050 Hz
// (half of CD rate) to stay inside CPU budget when 24 voices are mixed.

#pragma once
#include "fixed.hpp"

namespace acid::dsp {

class BDTBridge {
  public:
    // sampleRate in Hz. Typical: 22050 for PS1 SPU stream.
    explicit BDTBridge(int sampleRate);

    // tone   in [0, 1] — analog "tone" pot. Maps to tank Q (1..40-ish).
    //                    Higher = longer ringdown, more "boom".
    // decay  in [0, 1] — analog "decay" pot. Maps to env time constant
    //                    (~40 ms .. ~1500 ms).
    // tuning in [0, 1] — analog "tune" pot. Maps to f0 (~38 Hz .. ~85 Hz).
    void setParams(float tone, float decay, float tuning);

    // velocity in [0, 1]. 1.0 = full-rail trigger pulse.
    void trigger(float velocity);

    // Produce one sample of i16 audio (already saturated).
    i16 tick();

    bool isActive() const { return m_env > 32; }

  private:
    int m_sampleRate;

    // SVF (Chamberlin) state — band-pass output is the resonator we want.
    i32 m_low = 0;   // Q24
    i32 m_band = 0;  // Q24
    i32 m_f = 0;     // Q24 — freq coefficient
    i32 m_q = 0;     // Q24 — 1/Q (lower = more resonant)

    // Envelope (1-pole exponential decay)
    i32 m_env = 0;       // Q24, 0..1
    i32 m_envCoeff = 0;  // Q24, multiplier per sample

    // Pending trigger impulse, applied next tick().
    i32 m_pendingImpulse = 0;  // Q24
};

}  // namespace acid::dsp
