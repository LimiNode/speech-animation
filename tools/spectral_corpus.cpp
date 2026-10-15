#include "speech_animation/experimental/spectral.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using speech_animation::PcmChunk;
using speech_animation::Viseme;
using speech_animation::experimental::AudioReactiveVisemeAnalyzer;
using speech_animation::experimental::AudioReactiveVisemeConfig;
using speech_animation::experimental::AudioReactiveVisemeCue;
using speech_animation::experimental::AudioReactiveVisemeMode;

namespace {

std::uint16_t u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 2 > bytes.size()) throw std::runtime_error("truncated WAV");
    return static_cast<std::uint16_t>(bytes[offset]) |
           (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
}

std::uint32_t u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) throw std::runtime_error("truncated WAV");
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

std::string tag(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) throw std::runtime_error("truncated WAV");
    return std::string(reinterpret_cast<const char*>(bytes.data() + offset), 4);
}

struct Wav {
    std::uint32_t sample_rate = 0;
    std::uint16_t channels = 0;
    std::uint16_t format = 0;
    std::uint16_t bits = 0;
    std::size_t data_offset = 0;
    std::size_t data_size = 0;
};

Wav parse_wav(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < 12 || tag(bytes, 0) != "RIFF" || tag(bytes, 8) != "WAVE") {
        throw std::runtime_error("input is not a RIFF/WAVE file");
    }
    Wav wav;
    std::size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const auto id = tag(bytes, offset);
        const auto size = static_cast<std::size_t>(u32(bytes, offset + 4));
        offset += 8;
        if (size > bytes.size() - offset) throw std::runtime_error("truncated WAV chunk");
        if (id == "fmt ") {
            if (size < 16) throw std::runtime_error("invalid WAV fmt chunk");
            wav.format = u16(bytes, offset);
            wav.channels = u16(bytes, offset + 2);
            wav.sample_rate = u32(bytes, offset + 4);
            wav.bits = u16(bytes, offset + 14);
        }
        else if (id == "data") {
            wav.data_offset = offset;
            wav.data_size = size;
        }
        offset += size + (size & 1U);
    }
    if (wav.sample_rate == 0 || wav.channels == 0 || wav.data_size == 0) {
        throw std::runtime_error("WAV lacks format or data");
    }
    if (wav.format != 1 && wav.format != 3) {
        throw std::runtime_error("WAV must be PCM integer or IEEE float");
    }
    if (wav.format == 3 && wav.bits != 32) {
        throw std::runtime_error("IEEE float WAV must be 32-bit");
    }
    if (wav.format == 1 && wav.bits != 8 && wav.bits != 16 && wav.bits != 24 && wav.bits != 32) {
        throw std::runtime_error("unsupported PCM bit depth");
    }
    const auto bytes_per_sample = (wav.bits + 7U) / 8U;
    if (wav.data_size % (bytes_per_sample * wav.channels) != 0) {
        throw std::runtime_error("WAV data is not frame-aligned");
    }
    return wav;
}

float sample_at(const std::vector<std::uint8_t>& bytes, std::size_t offset,
                const Wav& wav) {
    if (wav.format == 3) {
        float value = 0.0F;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }
    if (wav.bits == 8) return (static_cast<float>(bytes[offset]) - 128.0F) / 128.0F;
    if (wav.bits == 16) {
        const auto raw = static_cast<std::int16_t>(u16(bytes, offset));
        return static_cast<float>(raw) / 32768.0F;
    }
    if (wav.bits == 24) {
        std::int32_t raw = static_cast<std::int32_t>(bytes[offset]) |
            (static_cast<std::int32_t>(bytes[offset + 1]) << 8) |
            (static_cast<std::int32_t>(bytes[offset + 2]) << 16);
        if ((raw & 0x00800000) != 0) raw |= ~0x00FFFFFF;
        return static_cast<float>(raw) / 8388608.0F;
    }
    return static_cast<float>(static_cast<std::int32_t>(u32(bytes, offset))) /
           2147483648.0F;
}

std::vector<float> decode_mono(const std::vector<std::uint8_t>& bytes, const Wav& wav) {
    const auto bytes_per_sample = (wav.bits + 7U) / 8U;
    const auto frame_bytes = bytes_per_sample * wav.channels;
    const auto frames = wav.data_size / frame_bytes;
    std::vector<float> result;
    result.reserve(frames);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float sum = 0.0F;
        for (std::size_t channel = 0; channel < wav.channels; ++channel) {
            sum += sample_at(bytes, wav.data_offset + frame * frame_bytes + channel * bytes_per_sample, wav);
        }
        result.push_back(sum / static_cast<float>(wav.channels));
    }
    return result;
}

const char* mode_name(AudioReactiveVisemeMode mode) {
    return mode == AudioReactiveVisemeMode::Amplitude ? "amplitude" : "spectral";
}

const char* viseme_name(Viseme value) {
    switch (value) {
    case Viseme::Neutral: return "neutral";
    case Viseme::Closed: return "closed";
    case Viseme::A: return "a";
    case Viseme::E: return "e";
    case Viseme::I: return "i";
    case Viseme::O: return "o";
    case Viseme::U: return "u";
    case Viseme::FV: return "fv";
    }
    return "neutral";
}

void emit(const AudioReactiveVisemeCue& cue, AudioReactiveVisemeMode mode) {
    std::cout << std::setprecision(9)
              << "{\"kind\":\"audio_span\",\"mode\":\"" << mode_name(mode)
              << "\",\"utterance_id\":" << cue.utterance_id
              << ",\"sample_begin\":" << cue.sample_begin
              << ",\"sample_count\":" << cue.sample_count
              << ",\"sample_rate\":" << cue.sample_rate
              << ",\"energy\":" << cue.energy
              << ",\"mouth_open\":" << cue.mouth_open
              << ",\"activity\":\""
              << (cue.activity == speech_animation::SpeechActivity::Active ? "active" : "silence")
              << "\",\"weights\":{\"a\":" << cue.weights.a
              << ",\"e\":" << cue.weights.e << ",\"i\":" << cue.weights.i
              << ",\"o\":" << cue.weights.o << ",\"u\":" << cue.weights.u << "}"
              << ",\"viseme\":";
    if (cue.viseme.has_value()) {
        std::cout << "{\"value\":\"" << viseme_name(cue.viseme->value)
                  << "\",\"confidence\":" << cue.viseme->confidence
                  << ",\"evidence\":\"audio_reactive\"}";
    }
    else std::cout << "null";
    std::cout << "}\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 3 || (std::string(argv[1]) != "--input" && std::string(argv[1]) != "-i")) {
            std::cerr << "usage: speech_animation_spectral_corpus --input file.wav [--mode amplitude|spectral]\n";
            return 2;
        }
        std::string input = argv[2];
        auto mode = AudioReactiveVisemeMode::Spectral;
        for (int index = 3; index + 1 < argc; ++index) {
            if (std::string(argv[index]) == "--mode") {
                const auto value = std::string(argv[index + 1]);
                if (value == "amplitude") mode = AudioReactiveVisemeMode::Amplitude;
                else if (value == "spectral") mode = AudioReactiveVisemeMode::Spectral;
                else throw std::runtime_error("mode must be amplitude or spectral");
                ++index;
            }
        }
        std::ifstream stream(input, std::ios::binary);
        if (!stream) throw std::runtime_error("cannot open input WAV");
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), {});
        const auto wav = parse_wav(bytes);
        const auto samples = decode_mono(bytes, wav);
        AudioReactiveVisemeConfig config;
        config.mode = mode;
        AudioReactiveVisemeAnalyzer analyzer(config);
        const PcmChunk chunk{1, 0, wav.sample_rate, samples.data(),
                             static_cast<std::uint32_t>(samples.size())};
        std::cout << "{\"kind\":\"metadata\",\"input\":\"" << input
                  << "\",\"sample_rate\":" << wav.sample_rate
                  << ",\"sample_count\":" << samples.size()
                  << ",\"channels\":" << wav.channels << "}\n";
        for (const auto& cue : analyzer.feed(chunk)) emit(cue, mode);
        for (const auto& cue : analyzer.flush()) emit(cue, mode);
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
