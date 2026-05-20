// Minimal mono 16-bit PCM wav writer. Host-only; not built for PS1.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace acid::host {

bool write_wav_mono16(const std::string &path, int sampleRate,
                      const std::vector<int16_t> &samples);

}  // namespace acid::host
