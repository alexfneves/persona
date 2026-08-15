#include "audio/wav.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <istream>
#include <ostream>
#include <stdexcept>
#include <vector>

namespace persona {

namespace {

uint16_t le16(const char* p) {
    return static_cast<uint16_t>(static_cast<uint8_t>(p[0])) |
           (static_cast<uint16_t>(static_cast<uint8_t>(p[1])) << 8);
}

uint32_t le32(const char* p) {
    return static_cast<uint32_t>(static_cast<uint8_t>(p[0])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[3])) << 24);
}

void read_bytes(std::istream& in, char* dst, size_t n) {
    in.read(dst, static_cast<std::streamsize>(n));
    if (!in || in.gcount() != static_cast<std::streamsize>(n)) {
        throw std::runtime_error("wav: unexpected end of file");
    }
}

// Linear resampler (v1: the fixture is already 16 kHz; good enough for other
// rates until a proper polyphase filter lands).
std::vector<float> resample_linear(const std::vector<float>& src, int src_rate, int dst_rate) {
    if (src_rate == dst_rate || src.empty()) {
        return src;
    }
    const size_t out_len = static_cast<size_t>(
        static_cast<double>(src.size()) * dst_rate / src_rate + 0.5);
    std::vector<float> out(out_len);
    const double step = static_cast<double>(src_rate) / dst_rate;
    for (size_t i = 0; i < out_len; ++i) {
        const double pos = static_cast<double>(i) * step;
        size_t i0 = static_cast<size_t>(pos);
        const size_t i1 = i0 + 1 < src.size() ? i0 + 1 : src.size() - 1;
        const double frac = pos - static_cast<double>(i0);
        out[i] = static_cast<float>(
            src[i0] * (1.0 - frac) + src[i1] * frac);
    }
    return out;
}

// Little-endian writers (the same byte order read_wav_f32's le16/le32 decode).
void write_le16(std::ostream& out, uint16_t v) {
    const char b[2] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff)};
    out.write(b, 2);
}

void write_le32(std::ostream& out, uint32_t v) {
    const char b[4] = {static_cast<char>(v & 0xff), static_cast<char>((v >> 8) & 0xff),
                       static_cast<char>((v >> 16) & 0xff), static_cast<char>((v >> 24) & 0xff)};
    out.write(b, 4);
}

// Canonical 16-bit PCM mono WAV (format 1) — readable by read_wav_f32 (T3),
// ffprobe/aplay, and the T0 audiocpp_cli.
void write_wav(std::ostream& out, int sample_rate, const std::vector<float>& samples) {
    if (sample_rate <= 0) {
        throw std::runtime_error("wav: invalid sample rate " + std::to_string(sample_rate));
    }
    const uint32_t data_size = static_cast<uint32_t>(samples.size()) * 2u;
    out.write("RIFF", 4);
    write_le32(out, 36u + data_size);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    write_le32(out, 16u);                                  // fmt chunk size
    write_le16(out, 1u);                                   // PCM
    write_le16(out, 1u);                                   // mono
    write_le32(out, static_cast<uint32_t>(sample_rate));
    write_le32(out, static_cast<uint32_t>(sample_rate) * 2u);  // byte rate
    write_le16(out, 2u);                                   // block align
    write_le16(out, 16u);                                  // bits per sample
    out.write("data", 4);
    write_le32(out, data_size);
    for (const float s : samples) {
        // float [-1,1] -> int16: clamp then round to nearest.
        const double v = s < -1.0 ? -1.0 : (s > 1.0 ? 1.0 : static_cast<double>(s));
        const int16_t i = static_cast<int16_t>(std::lround(v * 32767.0));
        write_le16(out, static_cast<uint16_t>(i));
    }
}

}  // namespace

std::vector<float> read_wav_f32(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("wav: cannot open '" + path + "'");
    }

    char hdr[12];
    read_bytes(in, hdr, 12);
    if (std::memcmp(hdr, "RIFF", 4) != 0 || std::memcmp(hdr + 8, "WAVE", 4) != 0) {
        throw std::runtime_error("wav: not a RIFF/WAVE file");
    }

    uint16_t audio_format = 0;
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    std::vector<char> data_chunk;
    bool have_fmt = false;
    bool have_data = false;

    // Walk chunks (fmt usually precedes data, but order is not guaranteed).
    char id[4];
    char size4[4];
    while (in.read(id, 4) && in.read(size4, 4)) {
        const uint32_t chunk_size = le32(size4);
        if (std::memcmp(id, "fmt ", 4) == 0 && !have_fmt) {
            // A fmt chunk smaller than 16 bytes cannot carry the fields we
            // parse below (format/channels/rate/bits); a crafted size < 16
            // would read past the end of the allocated buffer (review P2).
            if (chunk_size < 16) {
                throw std::runtime_error(
                    "wav: malformed fmt chunk (size " + std::to_string(chunk_size) +
                    " < 16)");
            }
            std::vector<char> fmt(chunk_size);
            read_bytes(in, fmt.data(), chunk_size);
            audio_format = le16(fmt.data());
            channels = le16(fmt.data() + 2);
            sample_rate = le32(fmt.data() + 4);
            bits_per_sample = le16(fmt.data() + 14);
            have_fmt = true;
        } else if (std::memcmp(id, "data", 4) == 0 && !have_data) {
            // Sanity cap on the data chunk: chunk_size is uint32_t, so a
            // malformed header could claim ~4 GiB and trigger a huge
            // allocation. 1 GiB of audio is ~6 hours at 16 kHz mono 16-bit —
            // far beyond any real fixture — so anything larger is corrupt.
            if (chunk_size > 1024u * 1024u * 1024u) {
                throw std::runtime_error(
                    "wav: data chunk too large (" + std::to_string(chunk_size) +
                    " bytes > 1 GiB)");
            }
            data_chunk.resize(chunk_size);
            read_bytes(in, data_chunk.data(), chunk_size);
            have_data = true;
        } else {
            in.ignore(static_cast<std::streamsize>(chunk_size));
        }
        if (chunk_size % 2 != 0) {
            in.ignore(1);  // chunks are padded to even length
        }
    }
    if (!have_fmt) {
        throw std::runtime_error("wav: missing fmt chunk");
    }
    if (!have_data) {
        throw std::runtime_error("wav: missing data chunk");
    }
    if (channels == 0 || sample_rate == 0) {
        throw std::runtime_error("wav: invalid channels/sample rate in header");
    }

    // Decode interleaved frames to mono float32 at the source rate.
    std::vector<float> mono;
    if (audio_format == 1 && bits_per_sample == 16) {
        const size_t nframes = (data_chunk.size() / 2) / channels;
        mono.resize(nframes);
        for (size_t f = 0; f < nframes; ++f) {
            int32_t acc = 0;
            for (uint16_t c = 0; c < channels; ++c) {
                const char* p = data_chunk.data() + (f * channels + c) * 2;
                acc += static_cast<int16_t>(le16(p));
            }
            mono[f] = static_cast<float>(acc) / static_cast<float>(channels) / 32768.0f;
        }
    } else if (audio_format == 3 && bits_per_sample == 32) {
        const size_t nframes = (data_chunk.size() / 4) / channels;
        mono.resize(nframes);
        for (size_t f = 0; f < nframes; ++f) {
            float acc = 0.0f;
            for (uint16_t c = 0; c < channels; ++c) {
                const char* p = data_chunk.data() + (f * channels + c) * 4;
                uint32_t raw = le32(p);
                float v;
                std::memcpy(&v, &raw, sizeof(v));
                acc += v;
            }
            mono[f] = acc / static_cast<float>(channels);
        }
    } else {
        throw std::runtime_error(
            "wav: unsupported encoding (format=" + std::to_string(audio_format) +
            ", bits=" + std::to_string(bits_per_sample) +
            "; supported: 16-bit PCM, float32)");
    }

    return resample_linear(mono, static_cast<int>(sample_rate), 16000);
}

void write_wav_stdout(int sample_rate, const std::vector<float>& samples) {
    write_wav(std::cout, sample_rate, samples);
    std::cout.flush();
}

void write_wav_file(const std::string& path, int sample_rate,
                    const std::vector<float>& samples) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("wav: cannot open '" + path + "' for writing");
    }
    write_wav(out, sample_rate, samples);
    if (!out) {
        throw std::runtime_error("wav: write failed for '" + path + "'");
    }
}

}  // namespace persona
