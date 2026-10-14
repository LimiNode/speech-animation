#pragma once

#include "speech_animation/types.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace speech_animation::experimental {

// This API is intentionally experimental. It provides audio-reactive shape
// hypotheses, not phoneme recognition or acoustic alignment.
enum class AudioReactiveVisemeMode {
    Amplitude,
    Spectral,
};

struct AudioReactiveVisemeConfig {
    AudioReactiveVisemeMode mode = AudioReactiveVisemeMode::Spectral;

    // A trailing causal analysis window and the sample-clock output hop.
    float window_ms = 30.0F;
    float output_hop_ms = 10.0F;
    std::uint32_t window_samples = 0;
    std::uint32_t output_hop_samples = 0;

    // RMS normalization and silence gate used by both variants.
    float energy_reference = 0.20F;
    float silence_threshold = 0.08F;
    float attack_ms = 8.0F;
    float release_ms = 90.0F;

    // Three physical bands, expressed in Hz rather than FFT-bin indices.
    float low_band_begin_hz = 250.0F;
    float low_band_end_hz = 700.0F;
    float mid_band_begin_hz = 700.0F;
    float mid_band_end_hz = 1800.0F;
    float high_band_begin_hz = 1800.0F;
    float high_band_end_hz = 3500.0F;

    // Audio-reactive evidence is deliberately capped below pronunciation
    // evidence: this is a shape hypothesis, never a known phoneme.
    float maximum_confidence = 0.60F;
};

struct AudioReactiveVisemeWeights {
    float a = 0.0F;
    float e = 0.0F;
    float i = 0.0F;
    float o = 0.0F;
    float u = 0.0F;
};

struct AudioReactiveVisemeCue {
    std::uint64_t utterance_id = 0;
    std::uint64_t sample_begin = 0;
    std::uint32_t sample_count = 0;
    std::uint32_t sample_rate = 0;

    float energy = 0.0F;
    float mouth_open = 0.0F;
    SpeechActivity activity = SpeechActivity::Silence;
    AudioReactiveVisemeWeights weights;

    // Empty means that the signal was silent/noisy or the spectral contrast
    // was too weak. A present estimate always carries AudioReactive evidence.
    std::optional<VisemeEstimate> viseme;
};

class AudioReactiveVisemeAnalyzer {
public:
    explicit AudioReactiveVisemeAnalyzer(AudioReactiveVisemeConfig config = {});

    AudioReactiveVisemeAnalyzer(const AudioReactiveVisemeAnalyzer&) = delete;
    AudioReactiveVisemeAnalyzer& operator=(const AudioReactiveVisemeAnalyzer&) = delete;

    // Consumes contiguous canonical PCM ranges. Callback arrival time is not
    // observed; output becomes available only after its causal window exists.
    std::vector<AudioReactiveVisemeCue> feed(const PcmChunk& chunk);

    // Emits one final partial range, if any observed samples remain.
    std::vector<AudioReactiveVisemeCue> flush();

    // Clears all state and returns to the uninitialized state.
    void reset() noexcept;

    const AudioReactiveVisemeConfig& config() const noexcept { return config_; }
    bool initialized() const noexcept { return initialized_; }
    bool flushed() const noexcept { return flushed_; }

private:
    struct BandEnergy {
        float low = 0.0F;
        float mid = 0.0F;
        float high = 0.0F;
    };

    void initialize(std::uint64_t first_sample, std::uint32_t sample_rate);
    void append_sample(float sample);
    AudioReactiveVisemeCue make_cue(std::uint64_t sample_begin,
                                    std::uint32_t sample_count,
                                    std::uint32_t frame_count);
    BandEnergy band_energy(std::uint32_t frame_count) const;
    std::vector<float> shape_scores(const BandEnergy& bands) const;
    static std::vector<float> normalize_scores(const std::vector<float>& scores);

    AudioReactiveVisemeConfig config_;
    bool initialized_ = false;
    bool flushed_ = false;
    std::uint64_t utterance_id_ = 0;
    std::uint64_t first_sample_ = 0;
    std::uint64_t next_sample_ = 0;
    std::uint64_t next_output_sample_ = 0;
    std::uint32_t sample_rate_ = 0;
    std::uint32_t window_samples_ = 0;
    std::uint32_t hop_samples_ = 0;
    std::uint64_t observed_count_ = 0;
    std::uint32_t stored_count_ = 0;
    std::uint32_t write_index_ = 0;
    std::vector<float> ring_;
    std::vector<float> window_weights_;
    float mouth_state_ = 0.0F;
};

} // namespace speech_animation::experimental
