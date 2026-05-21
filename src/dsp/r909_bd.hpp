// r909_bd.hpp — TR-909-style bass drum.
//
// The 909 BD is a hybrid: the same T-bridge resonator topology as the 808
// BD but with a much faster attack and a separate short "click" transient
// mixed into the output stage. That click is a key part of why the 909 BD
// sits in a mix differently — the body is rounder, but the attack is sharper.
//
// Reference (community RE, e.g. TR-909 service manual reproductions
// available on the web, plus discussion threads on Muff Wiggler / GS): the
// click is a quick decay envelope on a white-noise source, summed into the
// main tank output before the VCA. We do the same in discrete:
//
//   tank: same Chamberlin SVF as bd_tbridge.{hpp,cpp}, tuned higher (~70 Hz)
//   click: noise.hpp xorshift, gated by a ~5 ms exp decay envelope
//   sum:   tank_output * vca_env + click_output * click_env
//
// We keep tank_env and click_env as separate exp decays so the click can
// die fast (~5 ms) while the body rings on (~600 ms).

#pragma once
#include "fixed.hpp"
#include "noise.hpp"

namespace acid::dsp {

class R909BD {
  public:
    explicit R909BD(int sampleRate);

    // tuning in [0, 1]   — body f0 (~55 Hz .. ~110 Hz)
    // tone   in [0, 1]   — body Q (4 .. 30)
    // decay  in [0, 1]   — body env tau (~80 ms .. ~1200 ms)
    // attack in [0, 1]   — click amplitude (0 = no click)
    void setParams(float tuning, float tone, float decay, float attack);

    void trigger(float velocity);
    i16 tick();
    bool isActive() const { return m_env > 32 || m_clickEnv > 32; }

  private:
    int m_sampleRate;

    // Body T-bridge tank (Chamberlin SVF).
    i32 m_low = 0, m_band = 0;
    i32 m_f = 0, m_q = 0;
    i32 m_env = 0, m_envCoeff = 0;
    i32 m_pendingImpulse = 0;

    // Click stage.
    i32 m_clickEnv = 0, m_clickEnvCoeff = 0;
    i32 m_clickGain = 0;     // Q24 attack amount
    Noise m_noise;
};

}  // namespace acid::dsp
