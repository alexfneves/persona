#pragma once

#include <string>
#include <vector>

namespace persona {

// Decodes a WAV file into mono float32 samples resampled to 16 kHz — the
// format the ASR sessions expect (AudioBuffer{16000, 1, samples}).
// Supported encodings: 16-bit PCM (format 1) and float32 (format 3);
// multichannel input is downmixed to mono. Throws std::runtime_error on
// unreadable or unsupported files.
std::vector<float> read_wav_f32(const std::string& path);

}  // namespace persona
