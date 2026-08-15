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

// Encodes mono float32 samples as 16-bit PCM WAV (RIFF/fmt/data, mono, 16 bps)
// and writes them to stdout as binary — the composable form of `persona tts`
// (`persona tts ... | aplay`). Flushes stdout.
void write_wav_stdout(int sample_rate, const std::vector<float>& samples);

// Same encoding, written to a file. Throws std::runtime_error on open/write
// failure.
void write_wav_file(const std::string& path, int sample_rate,
                    const std::vector<float>& samples);

}  // namespace persona
