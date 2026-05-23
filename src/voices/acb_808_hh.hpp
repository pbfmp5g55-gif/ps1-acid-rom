// acb_808_hh.hpp — TR-808 Hi-Hat. Six non-harmonic squares summed,
// HPF → BPF cascade, exp env. 540/619/722/892/1024/1148 Hz family.
// Chamberlin SVF is just stable enough at our SR (44100 Hz) for the
// 6 kHz BPF center, so we skip the heavier ZDF-TPT form.

#pragma once
#include "dsp/fixed.hpp"

namespace acid::voices {

class Acb808Hh {
  public:
    static constexpr int SAMPLE_RATE = 44100;
    Acb808Hh();

    void setOpenness(acid::dsp::i32 opennessQ24);   // tau 50..500 ms
    void setBrightness(acid::dsp::i32 brightQ24);   // HPF/BPF center ±0.5 oct
    void setTune(acid::dsp::i32 tuneQ24);           // base freqs ±0.5 oct

    void trigger(acid::dsp::i32 velocityQ24);
    bool isActive() const { return m_env > 64; }

    acid::dsp::i16 tick();

  private:
    using i32 = acid::dsp::i32;
    using u32 = acid::dsp::u32;

    u32 m_phase[6] = {0};
    u32 m_step[6]  = {0};

    // HPF Chamberlin SVF (high tap).
    i32 m_hp_low = 0, m_hp_band = 0;
    i32 m_hp_f = 0, m_hp_q = 0;
    // BPF Chamberlin SVF (band tap).
    i32 m_bp_low = 0, m_bp_band = 0;
    i32 m_bp_f = 0, m_bp_q = 0;

    i32 m_env = 0, m_envCoeff = 0;
    i32 m_velocity = 0;
};

}  // namespace acid::voices
