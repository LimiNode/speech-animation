#pragma once

#include "speech_animation/types.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace speech_animation {

// Neutral pronunciation evidence supplied by a prepared-text frontend or by
// an application-owned pronunciation provider. The core does not interpret
// backend-specific text or stress types.
struct PronunciationUnit {
    std::string symbol;
    float confidence = 1.0F;
    float duration_weight = 1.0F;
};

// The media context needed to place a complete predicted sequence. The total
// duration is the known PCM duration, never a wall-clock estimate.
struct SpeechContext {
    std::uint64_t utterance_id = 0;
    std::uint64_t sample_begin = 0;
    std::uint32_t sample_rate = 0;
    std::uint64_t total_sample_count = 0;
};

struct PredictedVisemeSpec {
    Viseme value = Viseme::Neutral;
    float confidence = 0.0F;
    float duration_weight = 1.0F;
};

struct PredictedVisemeCue {
    std::uint64_t utterance_id = 0;
    std::uint64_t sample_begin = 0;
    std::uint32_t sample_count = 0;
    std::uint32_t sample_rate = 0;
    VisemeEstimate viseme{};
};

// Conservative, backend-independent pronunciation-to-viseme mapping. Unknown
// symbols intentionally become low-confidence Neutral rather than pretending
// that amplitude or an unsupported symbol identifies a precise mouth shape.
class PronunciationVisemeMapper {
public:
    static PredictedVisemeSpec map(const PronunciationUnit& unit);

    static std::vector<PredictedVisemeSpec> map_sequence(
        const std::vector<PronunciationUnit>& pronunciation);
};

using PronunciationToVisemeAdapter = PronunciationVisemeMapper;

// Places a known pronunciation sequence over a known PCM duration. This is a
// pure function of its inputs, so callback timing and chunking cannot affect
// the resulting cue timeline.
class PredictiveVisemePlanner {
public:
    PredictiveVisemePlanner() = default;
    explicit PredictiveVisemePlanner(SpeechContext context)
        : context_(context), has_context_(true) {}

    std::vector<PredictedVisemeCue> plan(
        const SpeechContext& context,
        const std::vector<PredictedVisemeSpec>& specs) const;

    std::vector<PredictedVisemeCue> plan(
        const std::vector<PredictedVisemeSpec>& specs) const;

private:
    SpeechContext context_{};
    bool has_context_ = false;
};

} // namespace speech_animation
