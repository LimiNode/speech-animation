#pragma once

#include "speech_animation/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace speech_animation {

/// Pronunciation-independent context supplied by an application/provider.
struct SpeechContext {
    std::uint64_t utterance_id = 0;
    std::uint32_t sample_rate = 0;
    std::uint64_t total_sample_count = 0;
    std::string pronunciation_text;
};

/// One expected mouth shape with a provider-assigned relative duration.
struct PredictedVisemeSpec {
    VisemeEstimate estimate;
    float duration_weight = 1.0F;
};

/// A sample-addressed predicted viseme interval.
struct PredictedVisemeCue {
    std::uint64_t sample_begin = 0;
    std::uint32_t sample_count = 0;
    VisemeEstimate estimate;
};

/// Allocates a known pronunciation/viseme sequence onto an utterance clock.
/// This class does not infer phonemes from text.
class PredictiveVisemePlanner {
public:
    explicit PredictiveVisemePlanner(SpeechContext context);

    PredictiveVisemePlanner(const PredictiveVisemePlanner&) = delete;
    PredictiveVisemePlanner& operator=(const PredictiveVisemePlanner&) = delete;

    /// Build contiguous cues covering the complete context sample range.
    std::vector<PredictedVisemeCue> plan(
        const std::vector<PredictedVisemeSpec>& sequence) const;

    /// Apply the cue with the greatest overlap to an audio span.
    SpeechAnimationSpan annotate(
        const SpeechAnimationSpan& span,
        const std::vector<PredictedVisemeCue>& cues) const;

    const SpeechContext& context() const noexcept { return context_; }

private:
    SpeechContext context_;
};

} // namespace speech_animation
