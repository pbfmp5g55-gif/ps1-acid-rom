// noise.hpp — deterministic pseudo-random noise source shared by SD / HH / CY /
// CP voices.
//
// In an analog TR-808 the noise generator is a reverse-biased BJT in
// avalanche breakdown — broadband white noise feeding several voice circuits.
// In our discrete model we use a 32-bit xorshift LFSR, which gives flat
// spectrum noise with period 2^32-1, plenty for short percussion hits.
//
// xorshift is chosen over std::mt19937 because (a) it's identical bit-for-bit
// on PS1 (R3000A integer math) and host, (b) it fits in 1 i32 of state and
// runs in 3 shifts + 3 xors per sample.
//
// Output is Q24 in [-1, 1).

#pragma once
#include "fixed.hpp"

namespace acid::dsp {

class Noise {
  public:
    explicit Noise(u32 seed = 0xACE1u) : m_state(seed ? seed : 0xACE1u) {}

    void reseed(u32 seed) { m_state = seed ? seed : 0xACE1u; }

    // One white-noise sample, Q24 in roughly [-1, 1).
    i32 tick() {
        m_state ^= m_state << 13;
        m_state ^= m_state >> 17;
        m_state ^= m_state << 5;
        // map u32 → Q24 signed centered noise.
        // top 25 bits as a signed int24, then shift up to Q24 alignment.
        i32 s = static_cast<i32>(m_state) >> 7;  // 25-bit signed
        return s;  // already roughly Q24-ish (top bit-range)
    }

  private:
    u32 m_state;
};

}  // namespace acid::dsp
