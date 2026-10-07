#pragma once

#include <cstdint>
#include <optional>

namespace speech_animation {

// A deliberately small, semantic taxonomy. Engine-specific mappings belong in adapters.
enum class Viseme {
    Neutral,
    Closed,
    A,
    E,
    I,
    O,
    U,
    FV,
};

// Evidence is part of the meaning of a cue, not merely metadata for a renderer.
enum class AnimationEvidence {
    AudioReactive,
    PredictedPronunciation,
    AcousticAligned,
};

enum class SpeechActivity {
    Silence,
    Active,
};

struct VisemeEstimate {
    Viseme value = Viseme::Neutral;
    float confidence = 0.0F;
    AnimationEvidence evidence = AnimationEvidence::AudioReactive;
};

// The canonical public result. All positions refer to the utterance's PCM clock.
struct SpeechAnimationSpan {
    std::uint64_t utterance_id = 0;
    std::uint64_t sample_begin = 0;
    std::uint32_t sample_count = 0;
    std::uint32_t sample_rate = 0;

    float energy = 0.0F;
    float mouth_open = 0.0F;
    std::optional<VisemeEstimate> viseme;

    // Audio-reactive state exposed alongside the canonical fields above.
    SpeechActivity activity = SpeechActivity::Silence;
    bool voiced = false;
};

// Generic PCM input. The buffer is borrowed for the duration of feed().
struct PcmChunk {
    std::uint64_t utterance_id = 0;
    std::uint64_t sample_begin = 0;
    std::uint32_t sample_rate = 0;
    const float* samples = nullptr;
    std::uint32_t sample_count = 0;
};

// Optional neutral progress evidence for a future predictive/alignment path.
// No backend-specific types belong in this header.
struct SpeechGenerationProgress {
    std::uint64_t sample_position = 0;
    float normalized_text_progress = 0.0F;
    float confidence = 0.0F;
};

} // namespace speech_animation
