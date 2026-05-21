// adpcm_psx.cpp — see adpcm_psx.hpp.

#include "adpcm_psx.hpp"

namespace acid::host::adpcm {

namespace {

// Encode one 28-sample block (mode 0, no filter) into 16 output bytes.
void encode_block(const int16_t in[28], uint8_t flags, uint8_t out[16]) {
    // Find max absolute sample.
    int max_abs = 1;
    for (int i = 0; i < 28; ++i) {
        int v = in[i] < 0 ? -in[i] : in[i];
        if (v > max_abs) max_abs = v;
    }
    // Largest shift such that max_abs fits in signed 4-bit after right shift.
    //   nibble = sample >> (12 - shift), needs |nibble| <= 7
    //   max_abs >> (12 - shift) <= 7
    //   12 - shift >= ceil(log2((max_abs + 7) / 8))
    //
    // Compute as: smallest k such that (7 << k) >= max_abs; shift = 12 - k.
    int k = 0;
    while ((7 << k) < max_abs && k < 12) ++k;
    int shift = 12 - k;
    if (shift < 0) shift = 0;
    if (shift > 12) shift = 12;

    out[0] = static_cast<uint8_t>(shift & 0x0F);  // filter mode 0
    out[1] = flags;

    for (int i = 0; i < 14; ++i) {
        int s0 = in[i * 2] >> (12 - shift);
        int s1 = in[i * 2 + 1] >> (12 - shift);
        if (s0 > 7) s0 = 7;
        if (s0 < -8) s0 = -8;
        if (s1 > 7) s1 = 7;
        if (s1 < -8) s1 = -8;
        out[2 + i] = static_cast<uint8_t>((s0 & 0x0F) | ((s1 & 0x0F) << 4));
    }
}

}  // namespace

std::vector<uint8_t> encode_one_shot(const std::vector<int16_t> &samples) {
    std::vector<int16_t> padded = samples;
    while (padded.size() % 28) padded.push_back(0);
    const int blocks = static_cast<int>(padded.size() / 28);
    std::vector<uint8_t> out(static_cast<size_t>(blocks) * 16);
    int16_t blockSamples[28];
    for (int b = 0; b < blocks; ++b) {
        for (int j = 0; j < 28; ++j) blockSamples[j] = padded[b * 28 + j];
        uint8_t flags = 0;
        if (b == blocks - 1) flags = FLAG_LOOP_END | FLAG_LOOP_ON;
        encode_block(blockSamples, flags, &out[b * 16]);
    }
    return out;
}

}  // namespace acid::host::adpcm
