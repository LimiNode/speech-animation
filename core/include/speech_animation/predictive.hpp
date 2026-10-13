#pragma once

#include "speech_animation/types.hpp"

#include <cstddef>
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
    // Absolute beginning of the utterance on the canonical PCM clock.
    std::uint64_t first_sample = 0;
};

// Neutral pronunciation evidence supplied by a prepared-text frontend or an
// application-owned pronunciation provider. Backend-specific pronunciation
// types must be converted before entering the core.
struct PronunciationUnit {
    std::string symbol;
    float confidence = 1.0F;
    float duration_weight = 1.0F;
};

/// One expected mouth shape with a provider-assigned relative duration.
struct PredictedVisemeSpec {
    VisemeEstimate estimate;
    float duration_weight = 1.0F;
};

class PronunciationVisemeMapper {
public:
    static PredictedVisemeSpec map(const PronunciationUnit& unit);
    static std::vector<PredictedVisemeSpec> map_sequence(
        const std::vector<PronunciationUnit>& pronunciation);
};

using PronunciationToVisemeAdapter = PronunciationVisemeMapper;

/// A sample-addressed predicted viseme interval.
struct PredictedVisemeCue {
    std::uint64_t sample_begin = 0;
    std::uint32_t sample_count = 0;
    VisemeEstimate estimate;
};

// Explicit policy for online placement. Without a nominal duration hint the
// planner refuses to invent timing and emits no predicted intervals.
struct OnlinePredictiveVisemePolicy {
    std::uint32_t samples_per_weight = 0;
    std::size_t max_prediction_units = 4096;
};

enum class OnlinePredictiveVisemeState {
    Idle,
    Active,
    Completed,
    Cancelled,
};

// Places a pronunciation sequence incrementally while the final PCM duration
// is unknown. It consumes sample ranges only; the audio producer remains
// untouched and no wall-clock timestamps are observed.
class OnlinePredictiveVisemePlanner {
public:
    explicit OnlinePredictiveVisemePlanner(
        OnlinePredictiveVisemePolicy policy = {});

    OnlinePredictiveVisemePlanner(const OnlinePredictiveVisemePlanner&) = delete;
    OnlinePredictiveVisemePlanner& operator=(const OnlinePredictiveVisemePlanner&) = delete;

    // Starts a new utterance. utterance_id must be unique for the generation;
    // the sequence is validated before state changes.
    void begin(std::uint64_t utterance_id,
               std::uint32_t sample_rate,
               std::uint64_t first_sample,
               const std::vector<PredictedVisemeSpec>& sequence);

    // Observes a contiguous PCM range and returns only newly publishable,
    // fully observed prediction intervals. A partial current interval is held
    // until complete(), so previously published ranges never move.
    std::vector<PredictedVisemeCue> observe_samples(
        std::uint64_t utterance_id,
        std::uint64_t first_sample,
        std::uint32_t sample_count);

    // Publishes a final clipped interval only when it intersects observed PCM.
    // Repeated completion is idempotent.
    std::vector<PredictedVisemeCue> complete(std::uint64_t utterance_id);

    // Stops publication without fabricating PCM or prediction intervals.
    // Repeated cancellation is idempotent.
    void cancel(std::uint64_t utterance_id);

    // Returns to Idle and clears all utterance state.
    void reset() noexcept;

    OnlinePredictiveVisemeState state() const noexcept { return state_; }
    std::uint64_t observed_sample_end() const noexcept { return observed_end_; }
    std::uint64_t published_sample_end() const noexcept { return published_end_; }
    const OnlinePredictiveVisemePolicy& policy() const noexcept { return policy_; }

private:
    struct Segment {
        std::uint64_t sample_begin = 0;
        std::uint32_t sample_count = 0;
        VisemeEstimate estimate;
    };

    std::vector<PredictedVisemeCue> publish_available(
        std::uint64_t observed_end,
        bool allow_partial);
    void validate_event_utterance(std::uint64_t utterance_id) const;

    OnlinePredictiveVisemePolicy policy_;
    OnlinePredictiveVisemeState state_ = OnlinePredictiveVisemeState::Idle;
    std::uint64_t utterance_id_ = 0;
    std::uint32_t sample_rate_ = 0;
    std::uint64_t first_sample_ = 0;
    std::uint64_t observed_end_ = 0;
    std::uint64_t published_end_ = 0;
    std::vector<Segment> segments_;
    std::size_t next_segment_ = 0;
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
