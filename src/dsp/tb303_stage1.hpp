// tb303_stage1.hpp — TB-303-style acid bass synth, stage 1.
//
// Reference (community RE: x0xb0x schematic walkthrough, Devil Fish notes,
// Antti Huovilainen's TB-303 paper, Yocto BOM): the 303 voice is
//   1× DCO     (saw or square, fast pitch swing for "slide")
//   1× VCF     (Sallen-Key / diode-clipper hybrid 24 dB/oct lowpass, with
//               heavy resonance and an envelope-modulated cutoff)
//   1× VCA     (current-controlled amplifier driven by a short decay env)
//   "accent"   sidechain that boosts both VCF env mod and VCA gain
//   "slide"    portamento ramping the DCO frequency between notes
//
// **Stage 1 (this file)** implements all of the above EXCEPT the VCF, which
// is collapsed to a single 1-pole resonant LPF (just enough to hear the
// envelope sweep and accent). Stage 2 (M6, tb303_stage2.*) will swap the
// 1-pole for a Stilson-Smith ZDF ladder approximation of the real Sallen-Key.
//
// We use 1-pole resonant here as the topology placeholder so the rest of the
// voice (DCO/env/accent/slide) is exercised end-to-end and unit-tested
// before the heavier filter math lands.

#pragma once
#include "fixed.hpp"

namespace acid::dsp {

enum class TB303Wave : int { Saw = 0, Square = 1 };

class TB303Stage1 {
  public:
    explicit TB303Stage1(int sampleRate);

    // Static / per-instrument knobs:
    //   waveform                — Saw or Square
    //   cutoff   in [0, 1]      — base VCF cutoff (~80 Hz .. ~4000 Hz)
    //   resonance in [0, 1]     — Q (1.0 .. ~12)
    //   envMod   in [0, 1]      — how much the env adds on top of cutoff
    //   decay    in [0, 1]      — VCF + VCA env tau (~50 ms .. ~1500 ms)
    //   accentAmount in [0, 1]  — global accent depth (master knob)
    void setParams(TB303Wave w, float cutoff, float resonance,
                   float envMod, float decay, float accentAmount);

    // Trigger a note.
    //   noteHz   — frequency to slide TO (current note's pitch)
    //   slide    — if true, DCO ramps from prevHz to noteHz over slideTime
    //              instead of jumping. Slide rate is fixed (~60 ms full ramp).
    //   accent   — if true, this note gets boosted env mod + VCA gain
    void noteOn(float noteHz, bool slide, bool accent);

    // Note off — releases the gate so env starts to decay all the way down.
    void noteOff();

    i16 tick();

    bool isActive() const { return m_envVca > 16; }

  private:
    int m_sampleRate;

    // DCO state (32-bit phase accumulator, like HH/CY).
    u32 m_phase = 0;
    u32 m_step = 0;
    u32 m_stepTarget = 0;  // slide target
    u32 m_stepRate = 0;    // steps per sample during slide
    bool m_sliding = false;
    TB303Wave m_wave = TB303Wave::Saw;

    // VCF (1-pole resonant LPF). State-variable form so we get the
    // resonance "peak" without the 1-pole's flat -6dB slope being entirely
    // bare. Stage 2 (M6) replaces this with a proper ladder.
    i32 m_lpLow = 0;
    i32 m_lpBand = 0;
    i32 m_baseCutoffHz = 0;  // float-domain stored as Q24 mult into Hz at runtime
    i32 m_envMod = 0;        // Q24 multiplier into env-modulated cutoff
    i32 m_resonanceQc = 0;   // Q24, 1/Q
    float m_cutoffParam = 0.5f;  // remembered so we can recompute on env change

    // VCF env (separate from VCA — VCF env is shorter and modulates cutoff).
    i32 m_envVcf = 0;
    i32 m_envVcfCoeff = 0;

    // VCA env.
    i32 m_envVca = 0;
    i32 m_envVcaCoeff = 0;
    i32 m_envVcaTarget = 0;  // 0 when noteOff, gate-high while held

    // Accent state for the current note (latched at noteOn).
    bool m_accent = false;
    i32 m_accentDepth = 0;  // Q24, depth knob value
};

}  // namespace acid::dsp
