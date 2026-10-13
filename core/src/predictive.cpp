#include "speech_animation/predictive.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <utility>

namespace speech_animation {
namespace {

float clamp01(float value) noexcept {
    if (!std::isfinite(value)) {
        return 0.0F;
    }
    return std::max(0.0F, std::min(1.0F, value));
}

std::string ascii_lower(std::string value) {
    for (char& character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte >= static_cast<unsigned char>('A') &&
            byte <= static_cast<unsigned char>('Z')) {
            character = static_cast<char>(byte + ('a' - 'A'));
        }
    }
    return value;
}

bool is_any(const std::string& symbol, std::initializer_list<const char*> values) {
    return std::any_of(values.begin(), values.end(), [&symbol](const char* value) {
        return symbol == value;
    });
}

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

PredictedVisemeSpec PronunciationVisemeMapper::map(const PronunciationUnit& unit) {
    const auto symbol = ascii_lower(unit.symbol);
    PredictedVisemeSpec result;
    result.estimate.confidence = clamp01(unit.confidence);
    result.estimate.evidence = AnimationEvidence::PredictedPronunciation;
    result.duration_weight = std::isfinite(unit.duration_weight) && unit.duration_weight > 0.0F
        ? unit.duration_weight
        : 1.0F;

    // Providers may use ASCII phoneme names (for example ARPAbet-like vowel
    // labels) or UTF-8 Cyrillic letters. Cyrillic letters are only an
    // approximate spelling-level input: they are not confirmed phonemes, and
    // context-dependent iotated vowels receive reduced confidence.
    if (symbol.empty()) {
        result.estimate.value = Viseme::Neutral;
        result.estimate.confidence *= 0.25F;
    } else if (is_any(symbol, {"sil", "sp", "pau", "pause", "<sil>"})) {
        result.estimate.value = Viseme::Closed;
        result.estimate.confidence *= 0.75F;
    } else if (is_any(symbol, {"p", "b", "m", "pcl", "bcl", "mcl"})) {
        result.estimate.value = Viseme::Closed;
    } else if (is_any(symbol, {"f", "v", "fcl", "vcl"})) {
        result.estimate.value = Viseme::FV;
    } else if (is_any(symbol, {"a", "aa", "ah", "\xD0\xB0"})) {
        result.estimate.value = Viseme::A;
    } else if (is_any(symbol, {"\xD1\x8F"})) {
        result.estimate.value = Viseme::A;
        result.estimate.confidence *= 0.75F;
    } else if (is_any(symbol, {"e", "ee", "eh", "ey", "\xD1\x8D"})) {
        result.estimate.value = Viseme::E;
    } else if (is_any(symbol, {"\xD0\xB5"})) {
        result.estimate.value = Viseme::E;
        result.estimate.confidence *= 0.75F;
    } else if (is_any(symbol, {"i", "ih", "iy", "\xD0\xB8"})) {
        result.estimate.value = Viseme::I;
    } else if (is_any(symbol, {"\xD1\x8B"})) {
        result.estimate.value = Viseme::I;
        result.estimate.confidence *= 0.75F;
    } else if (is_any(symbol, {"\xD0\xB9"})) {
        result.estimate.value = Viseme::Neutral;
        result.estimate.confidence *= 0.35F;
    } else if (is_any(symbol, {"o", "ao", "ow", "\xD0\xBE"})) {
        result.estimate.value = Viseme::O;
    } else if (is_any(symbol, {"\xD1\x91"})) {
        result.estimate.value = Viseme::O;
        result.estimate.confidence *= 0.75F;
    } else if (is_any(symbol, {"u", "uh", "uw", "\xD1\x83"})) {
        result.estimate.value = Viseme::U;
    } else if (is_any(symbol, {"\xD1\x8E"})) {
        result.estimate.value = Viseme::U;
        result.estimate.confidence *= 0.75F;
    } else {
        result.estimate.value = Viseme::Neutral;
        result.estimate.confidence *= 0.35F;
    }
    return result;
}

std::vector<PredictedVisemeSpec> PronunciationVisemeMapper::map_sequence(
    const std::vector<PronunciationUnit>& pronunciation) {
    std::vector<PredictedVisemeSpec> result;
    result.reserve(pronunciation.size());
    for (const auto& unit : pronunciation) {
        result.push_back(map(unit));
    }
    return result;
}

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
    if (context_.first_sample > std::numeric_limits<std::uint64_t>::max() -
                                  context_.total_sample_count) {
        throw std::invalid_argument("speech context sample range overflows");
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
    std::uint64_t sample_begin = context_.first_sample;
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
    const auto context_end = context_.first_sample + context_.total_sample_count;
    for (const auto& cue : cues) {
        const auto cue_end = end_sample(cue.sample_begin, cue.sample_count);
        validate_estimate(cue.estimate);
        if (cue.sample_begin < context_.first_sample || cue_end > context_end) {
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
