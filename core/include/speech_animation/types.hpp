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
};

// Generic normalized float PCM input. The buffer is borrowed for feed().
// Non-finite samples are treated as silence; finite values are clipped to [-1, 1].
struct PcmChunk {
    std::uint64_t utterance_id = 0;
    std::uint64_t sample_begin = 0;
    std::uint32_t sample_rate = 0;
    const float* samples = nullptr;
    std::uint32_t sample_count = 0;
};

} // namespace speech_animation
