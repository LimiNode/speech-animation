#pragma once

#include "speech_animation/types.hpp"

#include <vector>

namespace speech_animation {

struct AnalyzerConfig {
    // A zero sample rate means "take it from the first PcmChunk".
    std::uint32_t sample_rate = 0;

    // Time constants for causal one-pole smoothing. They do not add playback delay.
    float energy_attack_ms = 4.0F;
    float energy_release_ms = 70.0F;
    float mouth_attack_ms = 8.0F;
    float mouth_release_ms = 90.0F;

    // Adaptive peak normalization keeps different TTS/output gains usable.
    float normalizer_attack_ms = 80.0F;
    float normalizer_release_ms = 700.0F;
    float silence_threshold = 0.08F;
    float silence_floor = 1.0e-4F;

};

class SpeechAnimationAnalyzer {
public:
    explicit SpeechAnimationAnalyzer(AnalyzerConfig config = {});

    SpeechAnimationAnalyzer(const SpeechAnimationAnalyzer&) = delete;
    SpeechAnimationAnalyzer& operator=(const SpeechAnimationAnalyzer&) = delete;

    // Processes samples in canonical sample order. Arrival time is never observed.
    // Throws std::invalid_argument for malformed chunks and std::out_of_range for
    // a discontinuous/out-of-order sample position.
    std::vector<SpeechAnimationSpan> feed(const PcmChunk& chunk);

    // Per-sample processing has no pending look-ahead. flush() is provided so a
    // presentation/backend wrapper can use a uniform feed/flush lifecycle.
    std::vector<SpeechAnimationSpan> flush();

    // Starts a fresh utterance and clears all causal state.
    void reset();

    const AnalyzerConfig& config() const noexcept { return config_; }
    bool initialized() const noexcept { return initialized_; }
    std::uint64_t next_sample_position() const noexcept { return next_sample_position_; }

private:
    float coefficient(float milliseconds) const noexcept;
    SpeechAnimationSpan process_sample(std::uint64_t utterance_id,
                                       std::uint64_t sample_position,
                                       float sample);

    AnalyzerConfig config_;
    bool initialized_ = false;
    std::uint64_t utterance_id_ = 0;
    std::uint64_t next_sample_position_ = 0;
    std::uint32_t sample_rate_ = 0;

    float energy_state_ = 0.0F;
    float normalizer_state_ = 0.0F;
    float mouth_state_ = 0.0F;
};

using Analyzer = SpeechAnimationAnalyzer;

} // namespace speech_animation
