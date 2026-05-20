#include "wav_writer.hpp"
#include <cstdio>
#include <cstring>

namespace acid::host {

namespace {

void put_u32_le(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

void put_u16_le(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

}  // namespace

bool write_wav_mono16(const std::string &path, int sampleRate,
                      const std::vector<int16_t> &samples) {
    std::FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    uint8_t hdr[44];
    std::memcpy(hdr, "RIFF", 4);
    put_u32_le(hdr + 4, 36 + dataBytes);
    std::memcpy(hdr + 8, "WAVEfmt ", 8);
    put_u32_le(hdr + 16, 16);                                  // fmt chunk size
    put_u16_le(hdr + 20, 1);                                   // PCM
    put_u16_le(hdr + 22, 1);                                   // mono
    put_u32_le(hdr + 24, static_cast<uint32_t>(sampleRate));
    put_u32_le(hdr + 28, static_cast<uint32_t>(sampleRate) * 2);  // byterate
    put_u16_le(hdr + 32, 2);                                   // block align
    put_u16_le(hdr + 34, 16);                                  // bits/sample
    std::memcpy(hdr + 36, "data", 4);
    put_u32_le(hdr + 40, dataBytes);

    if (std::fwrite(hdr, 1, 44, f) != 44) { std::fclose(f); return false; }
    if (!samples.empty() &&
        std::fwrite(samples.data(), sizeof(int16_t), samples.size(), f) != samples.size()) {
        std::fclose(f); return false;
    }
    std::fclose(f);
    return true;
}

}  // namespace acid::host
