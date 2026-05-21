// adpcm_psx.hpp — PlayStation 1 ADPCM (also called "VAG body") encoder.
//
// Mode 0 (no predictor / filter). Each 28 samples → 16 bytes:
//   byte 0:        bits 0-3 = right-shift, bits 4-7 = filter mode (0)
//   byte 1:        flags. bit 0 = loop end, bit 1 = loop on, bit 2 = loop start
//   bytes 2..15:   28 signed-nibble samples (low nibble first)
//
// Decoder math:    out = (nibble << 12) >> shift  (filter=0)
// Encoder math:    nibble = clamp(sample >> (12 - shift), -8, +7)
//                  shift = largest value such that max_abs(block) fits in i4
//
// Filter modes 1-4 give better quality on smooth signals (e.g. saw bass) by
// predicting the next sample from the previous two and only encoding the
// residual. We deliberately skip them in this first M2 pass because mode 0
// is bit-trivial to encode (no per-block predictor search) and quality is
// still acceptable for short percussion hits and a saw bass at 22 kHz.

#pragma once
#include <cstdint>
#include <vector>

namespace acid::host::adpcm {

// Per-block flag bits.
constexpr uint8_t FLAG_LOOP_END   = 0x01;
constexpr uint8_t FLAG_LOOP_ON    = 0x02;
constexpr uint8_t FLAG_LOOP_START = 0x04;

// Encode N input samples (16-bit PCM) into a stream of 16-byte ADPCM blocks.
// Samples are padded to a multiple of 28 with zeros at the end.
// The last block carries flags = FLAG_LOOP_END | FLAG_LOOP_ON so SPU
// gracefully jumps to its loop target (typically a silent dummy block).
std::vector<uint8_t> encode_one_shot(const std::vector<int16_t> &samples);

}  // namespace acid::host::adpcm
