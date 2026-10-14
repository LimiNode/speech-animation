#include "speech_animation/experimental/spectral.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace speech_animation::experimental {
namespace {

constexpr float kPi = 3.14159265358979323846F;

float clamp01(float value) noexcept {
    return std::max(0.0F, std::min(1.0F, value));
}

float sanitize(float sample) noexcept {
    if (!std::isfinite(sample)) {
        return 0.0F;
    }
    return std::max(-1.0F, std::min(1.0F, sample));
}

float coefficient_for_samples(float milliseconds,
                              std::uint32_t sample_rate,
                              std::uint32_t sample_count) noexcept {
    if (milliseconds <= 0.0F || sample_rate == 0 || sample_count == 0) {
        return 1.0F;
    }
    const double tau = static_cast<double>(milliseconds) * 0.001 * sample_rate;
    return static_cast<float>(1.0 - std::exp(-static_cast<double>(sample_count) / tau));
}

} // namespace

AudioReactiveVisemeAnalyzer::AudioReactiveVisemeAnalyzer(
    AudioReactiveVisemeConfig config)
    : config_(config) {
    const bool finite = std::isfinite(config_.energy_reference) &&
                        std::isfinite(config_.silence_threshold) &&
                        std::isfinite(config_.attack_ms) &&
                        std::isfinite(config_.release_ms) &&
                        std::isfinite(config_.low_band_begin_hz) &&
                        std::isfinite(config_.low_band_end_hz) &&
                        std::isfinite(config_.mid_band_begin_hz) &&
                        std::isfinite(config_.mid_band_end_hz) &&
                        std::isfinite(config_.high_band_begin_hz) &&
                        std::isfinite(config_.high_band_end_hz) &&
                        std::isfinite(config_.maximum_confidence);
    const bool window_valid = config_.window_samples != 0 ||
                              (std::isfinite(config_.window_ms) &&
                               config_.window_ms > 0.0F);
    const bool hop_valid = config_.output_hop_samples != 0 ||
                           (std::isfinite(config_.output_hop_ms) &&
                            config_.output_hop_ms > 0.0F);
    const bool bands_valid = config_.low_band_begin_hz >= 0.0F &&
                             config_.low_band_begin_hz < config_.low_band_end_hz &&
                             config_.low_band_end_hz <= config_.mid_band_begin_hz &&
                             config_.mid_band_begin_hz < config_.mid_band_end_hz &&
                             config_.mid_band_end_hz <= config_.high_band_begin_hz &&
                             config_.high_band_begin_hz < config_.high_band_end_hz;
    if (!finite || !window_valid || !hop_valid || !bands_valid ||
        config_.energy_reference <= 0.0F || config_.silence_threshold < 0.0F ||
        config_.silence_threshold > 1.0F || config_.attack_ms < 0.0F ||
        config_.release_ms < 0.0F || config_.maximum_confidence < 0.0F ||
        config_.maximum_confidence > 1.0F) {
        throw std::invalid_argument("invalid experimental audio-reactive configuration");
    }
}

void AudioReactiveVisemeAnalyzer::initialize(std::uint64_t first_sample,
                                             std::uint32_t sample_rate) {
    const auto derive_samples = [sample_rate](float milliseconds,
                                              std::uint32_t explicit_samples,
                                              const char* name) {
        if (explicit_samples != 0) {
            return explicit_samples;
        }
        const double requested = static_cast<double>(milliseconds) * 0.001 * sample_rate;
        if (requested > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::invalid_argument(std::string(name) + " is too large for sample rate");
        }
        return std::max<std::uint32_t>(1,
            static_cast<std::uint32_t>(std::llround(requested)));
    };

    window_samples_ = derive_samples(config_.window_ms, config_.window_samples,
                                      "spectral window");
    hop_samples_ = derive_samples(config_.output_hop_ms, config_.output_hop_samples,
                                   "spectral output hop");
    if (window_samples_ == 0 || hop_samples_ == 0) {
        throw std::invalid_argument("spectral window and hop must be non-zero");
    }
    ring_.assign(window_samples_, 0.0F);
    window_weights_.resize(window_samples_);
    for (std::uint32_t index = 0; index < window_samples_; ++index) {
        window_weights_[index] = window_samples_ == 1U
            ? 1.0F
            : 0.5F * (1.0F - std::cos(2.0F * kPi * index /
                                         static_cast<float>(window_samples_ - 1U)));
    }
    initialized_ = true;
    first_sample_ = first_sample;
    next_sample_ = first_sample;
    next_output_sample_ = first_sample;
    sample_rate_ = sample_rate;
    observed_count_ = 0;
    stored_count_ = 0;
    write_index_ = 0;
    mouth_state_ = 0.0F;
}

void AudioReactiveVisemeAnalyzer::append_sample(float sample) {
    ring_[write_index_] = sanitize(sample);
    write_index_ = (write_index_ + 1U) % window_samples_;
    if (stored_count_ < window_samples_) {
        ++stored_count_;
    }
}

std::vector<AudioReactiveVisemeCue> AudioReactiveVisemeAnalyzer::feed(
    const PcmChunk& chunk) {
    if (flushed_) {
        throw std::logic_error("feed after flush requires reset()");
    }
    if (chunk.sample_rate == 0) {
        throw std::invalid_argument("PcmChunk.sample_rate must be non-zero");
    }
    if (chunk.sample_count != 0 && chunk.samples == nullptr) {
        throw std::invalid_argument("PcmChunk has a non-zero count and a null buffer");
    }
    if (chunk.sample_count >
        std::numeric_limits<std::uint64_t>::max() - chunk.sample_begin) {
        throw std::out_of_range("PcmChunk sample range overflows");
    }
    if (!initialized_) {
        if (chunk.utterance_id == 0) {
            throw std::invalid_argument("utterance_id must be non-zero");
        }
        utterance_id_ = chunk.utterance_id;
        initialize(chunk.sample_begin, chunk.sample_rate);
    } else {
        if (chunk.utterance_id != utterance_id_) {
            throw std::invalid_argument("utterance changed without reset()");
        }
        if (chunk.sample_rate != sample_rate_) {
            throw std::invalid_argument("sample rate changed within an utterance");
        }
        if (chunk.sample_begin != next_sample_) {
            throw std::out_of_range("PcmChunk is out of canonical sample order");
        }
    }

    std::vector<AudioReactiveVisemeCue> result;
    for (std::uint32_t index = 0; index < chunk.sample_count; ++index) {
        append_sample(chunk.samples[index]);
        ++observed_count_;
        ++next_sample_;

        while (observed_count_ >=
               (next_output_sample_ - first_sample_) + window_samples_) {
            result.push_back(make_cue(next_output_sample_, hop_samples_, window_samples_));
            next_output_sample_ += hop_samples_;
        }
    }
    return result;
}

std::vector<AudioReactiveVisemeCue> AudioReactiveVisemeAnalyzer::flush() {
    flushed_ = true;
    if (!initialized_ || next_output_sample_ >= next_sample_) {
        return {};
    }
    const auto remaining = next_sample_ - next_output_sample_;
    const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        remaining, std::numeric_limits<std::uint32_t>::max()));
    const auto frame_count = std::min(stored_count_, window_samples_);
    auto result = make_cue(next_output_sample_, count, frame_count);
    next_output_sample_ = next_sample_;
    return {result};
}

void AudioReactiveVisemeAnalyzer::reset() noexcept {
    initialized_ = false;
    flushed_ = false;
    utterance_id_ = 0;
    first_sample_ = 0;
    next_sample_ = 0;
    next_output_sample_ = 0;
    sample_rate_ = 0;
    window_samples_ = 0;
    hop_samples_ = 0;
    observed_count_ = 0;
    stored_count_ = 0;
    write_index_ = 0;
    ring_.clear();
    window_weights_.clear();
    mouth_state_ = 0.0F;
}

AudioReactiveVisemeCue AudioReactiveVisemeAnalyzer::make_cue(
    std::uint64_t sample_begin,
    std::uint32_t sample_count,
    std::uint32_t frame_count) {
    double square_sum = 0.0;
    const auto frame_start = (write_index_ + window_samples_ - frame_count) % window_samples_;
    for (std::uint32_t index = 0; index < frame_count; ++index) {
        const auto sample = ring_[(frame_start + index) % window_samples_];
        square_sum += static_cast<double>(sample) * static_cast<double>(sample);
    }
    const float rms = frame_count == 0
        ? 0.0F
        : static_cast<float>(std::sqrt(square_sum / static_cast<double>(frame_count)));
    const float energy = clamp01(rms / config_.energy_reference);
    const float target_mouth = energy <= config_.silence_threshold
        ? 0.0F
        : clamp01((energy - config_.silence_threshold) /
                   std::max(1.0e-6F, 1.0F - config_.silence_threshold));
    const float alpha = coefficient_for_samples(
        target_mouth > mouth_state_ ? config_.attack_ms : config_.release_ms,
        sample_rate_, sample_count);
    mouth_state_ += alpha * (target_mouth - mouth_state_);

    AudioReactiveVisemeCue cue;
    cue.utterance_id = utterance_id_;
    cue.sample_begin = sample_begin;
    cue.sample_count = sample_count;
    cue.sample_rate = sample_rate_;
    cue.energy = energy;
    cue.mouth_open = clamp01(mouth_state_);
    cue.activity = energy > config_.silence_threshold
        ? SpeechActivity::Active
        : SpeechActivity::Silence;

    const BandEnergy bands = config_.mode == AudioReactiveVisemeMode::Spectral
        ? band_energy(frame_count)
        : BandEnergy{};
    const auto scores = shape_scores(bands);
    const auto normalized = normalize_scores(scores);
    if (normalized.size() == 5U) {
        cue.weights = {normalized[0], normalized[1], normalized[2],
                       normalized[3], normalized[4]};
    }

    if (cue.activity == SpeechActivity::Active && normalized.size() == 5U) {
        std::size_t best = 0;
        std::size_t second = 1;
        for (std::size_t index = 1; index < normalized.size(); ++index) {
            if (normalized[index] > normalized[best]) {
                second = best;
                best = index;
            } else if (index != best && normalized[index] > normalized[second]) {
                second = index;
            }
        }
        const float contrast = std::max(0.0F, normalized[best] - normalized[second]);
        const float raw_confidence = config_.mode == AudioReactiveVisemeMode::Amplitude
            ? 0.20F + 0.10F * cue.energy
            : 2.0F * contrast;
        const float confidence = std::min(config_.maximum_confidence,
                                          clamp01(raw_confidence));
        if (confidence >= 0.12F) {
            static constexpr Viseme values[] = {
                Viseme::A, Viseme::E, Viseme::I, Viseme::O, Viseme::U};
            cue.viseme = VisemeEstimate{values[best], confidence,
                                         AnimationEvidence::AudioReactive};
        }
    }
    return cue;
}

AudioReactiveVisemeAnalyzer::BandEnergy
AudioReactiveVisemeAnalyzer::band_energy(std::uint32_t frame_count) const {
    BandEnergy result;
    if (frame_count == 0 || sample_rate_ == 0) {
        return result;
    }

    const auto power_at = [&](float frequency_hz) {
        const float nyquist = 0.5F * static_cast<float>(sample_rate_);
        if (frequency_hz <= 0.0F || frequency_hz >= nyquist) {
            return 0.0F;
        }
        const float omega = 2.0F * kPi * frequency_hz /
                            static_cast<float>(sample_rate_);
        const float coefficient_value = 2.0F * std::cos(omega);
        float previous = 0.0F;
        float previous_previous = 0.0F;
        const auto frame_start = (write_index_ + window_samples_ - frame_count) % window_samples_;
        for (std::uint32_t index = 0; index < frame_count; ++index) {
            const float window = frame_count == window_samples_
                ? window_weights_[index]
                : (frame_count == 1U
                    ? 1.0F
                    : 0.5F * (1.0F - std::cos(2.0F * kPi * index /
                                                 static_cast<float>(frame_count - 1U))));
            const float sample = ring_[(frame_start + index) % window_samples_] * window;
            const float current = sample + coefficient_value * previous - previous_previous;
            previous_previous = previous;
            previous = current;
        }
        const float power = previous_previous * previous_previous +
                            previous * previous - coefficient_value *
                            previous * previous_previous;
        return std::max(0.0F, power) /
               std::max(1.0F, static_cast<float>(frame_count * frame_count));
    };

    const auto band = [&](float begin_hz, float end_hz) {
        float sum = 0.0F;
        constexpr std::size_t samples = 3;
        for (std::size_t index = 0; index < samples; ++index) {
            const float fraction = (static_cast<float>(index) + 0.5F) /
                                   static_cast<float>(samples);
            sum += power_at(begin_hz + fraction * (end_hz - begin_hz));
        }
        return sum / static_cast<float>(samples);
    };

    result.low = band(config_.low_band_begin_hz, config_.low_band_end_hz);
    result.mid = band(config_.mid_band_begin_hz, config_.mid_band_end_hz);
    result.high = band(config_.high_band_begin_hz, config_.high_band_end_hz);
    return result;
}

std::vector<float> AudioReactiveVisemeAnalyzer::shape_scores(
    const BandEnergy& bands) const {
    if (config_.mode == AudioReactiveVisemeMode::Amplitude) {
        // This is deliberately coarse: it uses only amplitude dynamics and
        // avoids claiming that similarly loud vowels were recognized.
        const float broad = clamp01(mouth_state_);
        if (broad < 0.33F) {
            return {0.15F, 0.15F, 0.10F, 0.30F, 0.30F};
        }
        if (broad < 0.66F) {
            return {0.45F, 0.30F, 0.10F, 0.10F, 0.05F};
        }
        return {0.45F, 0.10F, 0.05F, 0.35F, 0.05F};
    }
    const float total = bands.low + bands.mid + bands.high;
    if (total <= 1.0e-12F) {
        return std::vector<float>(5, 0.0F);
    }
    const float low = bands.low / total;
    const float mid = bands.mid / total;
    const float high = bands.high / total;
    // These are shape hypotheses based on relative band energy, not vowel
    // classifiers. The coefficients are intentionally simple and inspectable.
    return {
        0.60F * low + 0.30F * mid + 0.10F * high, // A
        0.10F * low + 0.55F * mid + 0.35F * high, // E
        0.05F * low + 0.25F * mid + 0.70F * high, // I
        0.70F * low + 0.25F * mid + 0.05F * high, // O
        0.45F * low + 0.10F * mid + 0.45F * high, // U
    };
}

std::vector<float> AudioReactiveVisemeAnalyzer::normalize_scores(
    const std::vector<float>& scores) {
    float total = 0.0F;
    for (const float score : scores) {
        total += std::max(0.0F, score);
    }
    if (total <= 1.0e-12F) {
        return std::vector<float>(scores.size(), 0.0F);
    }
    std::vector<float> normalized;
    normalized.reserve(scores.size());
    for (const float score : scores) {
        normalized.push_back(std::max(0.0F, score) / total);
    }
    return normalized;
}

} // namespace speech_animation::experimental
