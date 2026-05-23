// adpcm_encode.hpp — Encode 28-sample PCM blocks to PSX ADPCM (mode 0, no
// predictor). Used by the streaming engine to compress freshly-rendered
// audio before DMA-ing it into SPU RAM.
//
// Bit-exact match with host_tests/adpcm_psx.cpp so what we hear in MinGW
// matches what the PS1 SPU plays.

#pragma once
#include <cstdint>

namespace acid::audio::adpcm {

// PSX ADPCM block flag bits (byte 1 of each 16-byte block).
constexpr uint8_t FLAG_LOOP_END   = 0x01;  // last block; SPU stops or loops
constexpr uint8_t FLAG_LOOP_ON    = 0x02;  // combined with LOOP_END = jump to repeat addr
constexpr uint8_t FLAG_LOOP_START = 0x04;  // mark this block as the repeat point

// Encode one 28-sample block into 16 ADPCM bytes (mode 0 = no predictor).
// `flags` is OR'd into byte 1.
//
// Adapted from host_tests/adpcm_psx.cpp. Integer-only, no STL, no allocation —
// safe to call from the PS1 audio path.
inline void encode_block(const int16_t in[28], uint8_t flags, uint8_t out[16]) {
    // Find max absolute sample to pick the per-block exponent.
    int max_abs = 1;
    for (int i = 0; i < 28; ++i) {
        int v = in[i] < 0 ? -in[i] : in[i];
        if (v > max_abs) max_abs = v;
    }
    // Smallest k such that (7 << k) >= max_abs; shift = 12 - k.
    int k = 0;
    while ((7 << k) < max_abs && k < 12) ++k;
    int shift = 12 - k;
    if (shift < 0) shift = 0;
    if (shift > 12) shift = 12;

    out[0] = static_cast<uint8_t>(shift & 0x0F);  // filter mode 0 in upper nibble
    out[1] = flags;

    for (int i = 0; i < 14; ++i) {
        int s0 = in[i * 2]     >> (12 - shift);
        int s1 = in[i * 2 + 1] >> (12 - shift);
        if (s0 >  7) s0 =  7;
        if (s0 < -8) s0 = -8;
        if (s1 >  7) s1 =  7;
        if (s1 < -8) s1 = -8;
        out[2 + i] = static_cast<uint8_t>((s0 & 0x0F) | ((s1 & 0x0F) << 4));
    }
}

}  // namespace acid::audio::adpcm
