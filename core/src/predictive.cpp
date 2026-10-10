#include "speech_animation/predictive.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace speech_animation {
namespace {

void validate_estimate(const VisemeEstimate& estimate) {
    if (!std::isfinite(estimate.confidence) ||
        estimate.confidence < 0.0F || estimate.confidence > 1.0F) {
        throw std::invalid_argument("predicted viseme confidence must be in [0, 1]");
    }
    if (estimate.evidence != AnimationEvidence::PredictedPronunciation) {
        throw std::invalid_argument(
            "predictive viseme cues require PredictedPronunciation evidence");
    }
}

std::uint64_t end_sample(std::uint64_t begin, std::uint32_t count) {
    if (count == 0 || begin > std::numeric_limits<std::uint64_t>::max() - count) {
        throw std::invalid_argument("predicted viseme cue has an invalid sample range");
    }
    return begin + count;
}

} // namespace

PredictiveVisemePlanner::PredictiveVisemePlanner(SpeechContext context)
    : context_(std::move(context)) {
    if (context_.sample_rate == 0 || context_.total_sample_count == 0 ||
        context_.total_sample_count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(
            "speech context requires a sample rate and uint32 sample count");
    }
    if (context_.utterance_id == 0) {
        throw std::invalid_argument("speech context requires a non-zero utterance id");
    }
}

std::vector<PredictedVisemeCue> PredictiveVisemePlanner::plan(
    const std::vector<PredictedVisemeSpec>& sequence) const {
    if (sequence.empty() || sequence.size() > context_.total_sample_count) {
        throw std::invalid_argument("invalid predictive viseme sequence length");
    }

    double total_weight = 0.0;
    for (const auto& spec : sequence) {
        validate_estimate(spec.estimate);
        if (!std::isfinite(spec.duration_weight) || spec.duration_weight <= 0.0F) {
            throw std::invalid_argument("predictive viseme duration weight must be positive");
        }
        total_weight += static_cast<double>(spec.duration_weight);
    }

    const auto sample_count = context_.total_sample_count;
    std::vector<std::uint32_t> counts(sequence.size(), 1);
    const auto remaining = sample_count - sequence.size();
    std::uint64_t assigned = 0;
    std::vector<double> fractions(sequence.size(), 0.0);
    for (std::size_t index = 0; index < sequence.size(); ++index) {
        const double exact = static_cast<double>(remaining) *
            static_cast<double>(sequence[index].duration_weight) / total_weight;
        const auto whole = static_cast<std::uint64_t>(std::floor(exact));
        counts[index] += static_cast<std::uint32_t>(whole);
        assigned += whole;
        fractions[index] = exact - static_cast<double>(whole);
    }
    auto leftovers = remaining - assigned;
    std::vector<std::size_t> order(sequence.size());
    for (std::size_t index = 0; index < order.size(); ++index) {
        order[index] = index;
    }
    std::stable_sort(order.begin(), order.end(), [&fractions](std::size_t left, std::size_t right) {
        return fractions[left] > fractions[right];
    });
    for (std::size_t index = 0; leftovers != 0; ++index, --leftovers) {
        ++counts[order[index % order.size()]];
    }

    std::vector<PredictedVisemeCue> result;
    result.reserve(sequence.size());
    std::uint64_t sample_begin = 0;
    for (std::size_t index = 0; index < sequence.size(); ++index) {
        result.push_back({sample_begin, counts[index], sequence[index].estimate});
        sample_begin += counts[index];
    }
    return result;
}

SpeechAnimationSpan PredictiveVisemePlanner::annotate(
    const SpeechAnimationSpan& span,
    const std::vector<PredictedVisemeCue>& cues) const {
    if (span.utterance_id != context_.utterance_id ||
        span.sample_rate != context_.sample_rate || span.sample_count == 0) {
        throw std::invalid_argument("speech span does not match predictive context");
    }
    const auto span_end = end_sample(span.sample_begin, span.sample_count);
    SpeechAnimationSpan result = span;
    result.viseme.reset();
    std::uint64_t best_overlap = 0;
    for (const auto& cue : cues) {
        const auto cue_end = end_sample(cue.sample_begin, cue.sample_count);
        validate_estimate(cue.estimate);
        if (cue_end > context_.total_sample_count) {
            throw std::invalid_argument("predicted viseme cue exceeds speech context");
        }
        const auto overlap_begin = std::max(span.sample_begin, cue.sample_begin);
        const auto overlap_end = std::min(span_end, cue_end);
        if (overlap_end > overlap_begin && overlap_end - overlap_begin > best_overlap) {
            best_overlap = overlap_end - overlap_begin;
            result.viseme = cue.estimate;
        }
    }
    return result;
}

} // namespace speech_animation
