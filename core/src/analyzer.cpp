#include "speech_animation/analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
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
                        std::isfinite(config_.silence_floor) &&
                        std::isfinite(config_.output_hop_ms);
    if (!finite || config_.energy_attack_ms < 0.0F || config_.energy_release_ms < 0.0F ||
        config_.mouth_attack_ms < 0.0F || config_.mouth_release_ms < 0.0F ||
        config_.normalizer_attack_ms < 0.0F || config_.normalizer_release_ms < 0.0F ||
        config_.silence_threshold < 0.0F || config_.silence_threshold > 1.0F ||
        config_.silence_floor < 0.0F || config_.output_hop_ms <= 0.0F) {
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
        sample_rate_ = chunk.sample_rate;
        initialize_hop_grid(chunk.sample_begin);
        initialized_ = true;
        utterance_id_ = chunk.utterance_id;
        next_sample_position_ = chunk.sample_begin;
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
    const std::size_t possible_spans =
        (static_cast<std::size_t>(current_hop_count_) + chunk.sample_count) /
        static_cast<std::size_t>(hop_samples_);
    result.reserve(possible_spans);
    for (std::size_t i = 0; i < chunk.sample_count; ++i) {
        const auto sample_span = process_sample(utterance_id_, next_sample_position_, chunk.samples[i]);
        accumulate(sample_span);
        ++next_sample_position_;
        if (current_hop_count_ == hop_samples_) {
            result.push_back(finish_hop());
        }
    }
    return result;
}

std::vector<SpeechAnimationSpan> SpeechAnimationAnalyzer::flush() {
    if (!initialized_ || current_hop_count_ == 0) {
        return {};
    }
    return {finish_hop()};
}

void SpeechAnimationAnalyzer::reset() {
    initialized_ = false;
    utterance_id_ = 0;
    next_sample_position_ = 0;
    sample_rate_ = 0;
    hop_samples_ = 0;
    current_hop_begin_ = 0;
    current_hop_count_ = 0;
    energy_sum_ = 0.0;
    mouth_sum_ = 0.0;
    active_count_ = 0;
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

void SpeechAnimationAnalyzer::initialize_hop_grid(std::uint64_t first_sample_position) {
    if (config_.output_hop_samples != 0) {
        hop_samples_ = config_.output_hop_samples;
    } else {
        const double requested = static_cast<double>(config_.output_hop_ms) * 0.001 *
                                 static_cast<double>(sample_rate_);
        if (requested > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::invalid_argument("output hop is too large for the sample rate");
        }
        hop_samples_ = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::llround(requested)));
    }
    current_hop_begin_ = first_sample_position;
    clear_hop_accumulator();
}

void SpeechAnimationAnalyzer::accumulate(const SpeechAnimationSpan& sample_span) noexcept {
    if (current_hop_count_ == 0) {
        current_hop_begin_ = sample_span.sample_begin;
    }
    energy_sum_ += static_cast<double>(sample_span.energy);
    mouth_sum_ += static_cast<double>(sample_span.mouth_open);
    if (sample_span.activity == SpeechActivity::Active) {
        ++active_count_;
    }
    ++current_hop_count_;
}

SpeechAnimationSpan SpeechAnimationAnalyzer::finish_hop() {
    SpeechAnimationSpan span;
    span.utterance_id = utterance_id_;
    span.sample_begin = current_hop_begin_;
    span.sample_count = current_hop_count_;
    span.sample_rate = sample_rate_;
    span.energy = static_cast<float>(energy_sum_ / static_cast<double>(current_hop_count_));
    span.mouth_open = static_cast<float>(mouth_sum_ / static_cast<double>(current_hop_count_));
    span.activity = active_count_ == 0 ? SpeechActivity::Silence : SpeechActivity::Active;
    span.viseme.reset();
    clear_hop_accumulator();
    current_hop_begin_ = next_sample_position_;
    return span;
}

void SpeechAnimationAnalyzer::clear_hop_accumulator() noexcept {
    current_hop_count_ = 0;
    energy_sum_ = 0.0;
    mouth_sum_ = 0.0;
    active_count_ = 0;
}

} // namespace speech_animation
