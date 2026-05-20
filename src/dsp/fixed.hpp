// fixed.hpp — Q15 / Q24 fixed-point helpers for PS1-friendly DSP.
//
// The R3000A has no FPU. All audio math runs in integer fixed point.
// We use:
//   - i16 for audio samples (the SPU streams 16-bit PCM)
//   - i32 for accumulators / intermediate state
//   - Q24 (1.31 internally clamped) for filter state, gives ~145 dB headroom
//     before saturation when summed across a few voices
//
// Host tests compile this exact file under x86_64 and the DSP rounds to the
// same bit-exact values as the PS1 build — that is the whole point of doing
// integer math everywhere.

#pragma once
#include <cstdint>

namespace acid::dsp {

using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using u32 = uint32_t;

// Q24: 8.24 signed fixed-point. Range ±128.0, resolution ~6e-8.
// Wide enough for filter feedback without losing low-end decay tails.
constexpr int Q24_SHIFT = 24;

constexpr i32 to_q24(float x) { return static_cast<i32>(x * (1 << Q24_SHIFT)); }

constexpr float q24_to_f(i32 x) {
    return static_cast<float>(x) / static_cast<float>(1 << Q24_SHIFT);
}

// Multiply two Q24 values → Q24. Uses i64 to avoid overflow.
inline i32 mul_q24(i32 a, i32 b) {
    return static_cast<i32>((static_cast<i64>(a) * b) >> Q24_SHIFT);
}

// Clamp helper for final i16 audio output.
inline i16 sat16(i32 x) {
    if (x >  32767) return  32767;
    if (x < -32768) return -32768;
    return static_cast<i16>(x);
}

}  // namespace acid::dsp
