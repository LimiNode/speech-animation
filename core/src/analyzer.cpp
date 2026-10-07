#include "speech_animation/analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace speech_animation {
namespace {

float clamp01(float value) noexcept {
    return std::max(0.0F, std::min(1.0F, value));
}

float one_pole_coefficient(float milliseconds, std::uint32_t sample_rate) noexcept {
    if (milliseconds <= 0.0F || sample_rate == 0) {
        return 1.0F;
    }
    // This is the exact per-sample coefficient of a continuous-time one-pole.
    const float tau_samples = milliseconds * 0.001F * static_cast<float>(sample_rate);
    return 1.0F - std::exp(-1.0F / std::max(1.0F, tau_samples));
}

} // namespace

SpeechAnimationAnalyzer::SpeechAnimationAnalyzer(AnalyzerConfig config)
    : config_(config) {
    const bool finite = std::isfinite(config_.energy_attack_ms) &&
                        std::isfinite(config_.energy_release_ms) &&
                        std::isfinite(config_.mouth_attack_ms) &&
                        std::isfinite(config_.mouth_release_ms) &&
                        std::isfinite(config_.normalizer_attack_ms) &&
                        std::isfinite(config_.normalizer_release_ms) &&
                        std::isfinite(config_.silence_threshold) &&
                        std::isfinite(config_.silence_floor);
    if (!finite || config_.energy_attack_ms < 0.0F || config_.energy_release_ms < 0.0F ||
        config_.mouth_attack_ms < 0.0F || config_.mouth_release_ms < 0.0F ||
        config_.normalizer_attack_ms < 0.0F || config_.normalizer_release_ms < 0.0F ||
        config_.silence_threshold < 0.0F || config_.silence_threshold > 1.0F ||
        config_.silence_floor < 0.0F) {
        throw std::invalid_argument("invalid speech animation analyzer configuration");
    }
}

float SpeechAnimationAnalyzer::coefficient(float milliseconds) const noexcept {
    return one_pole_coefficient(milliseconds, sample_rate_);
}

std::vector<SpeechAnimationSpan> SpeechAnimationAnalyzer::feed(const PcmChunk& chunk) {
    if (chunk.sample_count != 0 && chunk.samples == nullptr) {
        throw std::invalid_argument("PcmChunk has a non-zero count and a null buffer");
    }
    if (chunk.sample_rate == 0) {
        throw std::invalid_argument("PcmChunk.sample_rate must be non-zero");
    }
    if (config_.sample_rate != 0 && chunk.sample_rate != config_.sample_rate) {
        throw std::invalid_argument("PcmChunk sample rate differs from AnalyzerConfig");
    }

    if (!initialized_) {
        initialized_ = true;
        utterance_id_ = chunk.utterance_id;
        next_sample_position_ = chunk.sample_begin;
        sample_rate_ = chunk.sample_rate;
    } else {
        if (chunk.utterance_id != utterance_id_) {
            throw std::invalid_argument("utterance changed without reset()");
        }
        if (chunk.sample_rate != sample_rate_) {
            throw std::invalid_argument("sample rate changed within an utterance");
        }
        if (chunk.sample_begin != next_sample_position_) {
            throw std::out_of_range("PcmChunk is out of canonical sample order");
        }
    }

    std::vector<SpeechAnimationSpan> result;
    result.reserve(chunk.sample_count);
    for (std::size_t i = 0; i < chunk.sample_count; ++i) {
        result.push_back(process_sample(utterance_id_, next_sample_position_, chunk.samples[i]));
        ++next_sample_position_;
    }
    return result;
}

std::vector<SpeechAnimationSpan> SpeechAnimationAnalyzer::flush() {
    return {};
}

void SpeechAnimationAnalyzer::reset() {
    initialized_ = false;
    utterance_id_ = 0;
    next_sample_position_ = 0;
    sample_rate_ = 0;
    energy_state_ = 0.0F;
    normalizer_state_ = 0.0F;
    mouth_state_ = 0.0F;
}

SpeechAnimationSpan SpeechAnimationAnalyzer::process_sample(std::uint64_t utterance_id,
                                                            std::uint64_t sample_position,
                                                            float sample) {
    if (!std::isfinite(sample)) {
        sample = 0.0F;
    }
    const float magnitude = std::min(1.0F, std::fabs(sample));
    const float target_power = magnitude * magnitude;
    const float energy_alpha = target_power > energy_state_
        ? coefficient(config_.energy_attack_ms)
        : coefficient(config_.energy_release_ms);
    energy_state_ += energy_alpha * (target_power - energy_state_);
    const float raw_energy = std::sqrt(std::max(0.0F, energy_state_));

    const float normalizer_alpha = raw_energy > normalizer_state_
        ? coefficient(config_.normalizer_attack_ms)
        : coefficient(config_.normalizer_release_ms);
    normalizer_state_ += normalizer_alpha * (raw_energy - normalizer_state_);
    const float denominator = std::max(config_.silence_floor, normalizer_state_);
    const float normalized_energy = clamp01(raw_energy / denominator);

    const float target_mouth = normalized_energy <= config_.silence_threshold
        ? 0.0F
        : clamp01((normalized_energy - config_.silence_threshold) /
                   std::max(1.0e-6F, 1.0F - config_.silence_threshold));
    const float mouth_alpha = target_mouth > mouth_state_
        ? coefficient(config_.mouth_attack_ms)
        : coefficient(config_.mouth_release_ms);
    mouth_state_ += mouth_alpha * (target_mouth - mouth_state_);

    const bool active = normalized_energy > config_.silence_threshold;
    SpeechAnimationSpan span;
    span.utterance_id = utterance_id;
    span.sample_begin = sample_position;
    span.sample_count = 1;
    span.sample_rate = sample_rate_;
    span.energy = normalized_energy;
    span.mouth_open = clamp01(mouth_state_);
    span.activity = active ? SpeechActivity::Active : SpeechActivity::Silence;
    span.viseme.reset();
    return span;
}

} // namespace speech_animation
