#include "speech_animation/predictive.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <initializer_list>
#include <limits>
#include <numeric>
#include <stdexcept>

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
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

bool is_any(const std::string& symbol, std::initializer_list<const char*> values) {
    return std::any_of(values.begin(), values.end(), [&symbol](const char* value) {
        return symbol == value;
    });
}

} // namespace

PredictedVisemeSpec PronunciationVisemeMapper::map(const PronunciationUnit& unit) {
    const auto symbol = ascii_lower(unit.symbol);
    PredictedVisemeSpec result;
    result.confidence = clamp01(unit.confidence);
    result.duration_weight = std::isfinite(unit.duration_weight) && unit.duration_weight > 0.0F
        ? unit.duration_weight
        : 1.0F;

    if (symbol.empty() || is_any(symbol, {"sil", "sp", "pau", "pause", "<sil>"})) {
        result.value = Viseme::Closed;
        // Silence markers are useful for timing but are not a confident
        // pronunciation claim.
        result.confidence *= 0.75F;
        return result;
    }
    if (is_any(symbol, {"p", "b", "m", "pcl", "bcl", "mcl"})) {
        result.value = Viseme::Closed;
    } else if (is_any(symbol, {"f", "v", "fcl", "vcl"})) {
        result.value = Viseme::FV;
    } else if (is_any(symbol, {"a", "aa", "ah", "ɑ", "ɐ", "а", "я"})) {
        result.value = Viseme::A;
    } else if (is_any(symbol, {"e", "ee", "eh", "ɛ", "eɪ", "э", "е", "ё"})) {
        result.value = Viseme::E;
    } else if (is_any(symbol, {"i", "ih", "iy", "ɪ", "iː", "и", "й"})) {
        result.value = Viseme::I;
    } else if (is_any(symbol, {"o", "ao", "ow", "ɔ", "о", "ё"})) {
        result.value = Viseme::O;
    } else if (is_any(symbol, {"u", "uh", "uw", "ʊ", "uː", "у", "ю"})) {
        result.value = Viseme::U;
    } else {
        result.value = Viseme::Neutral;
        result.confidence *= 0.35F;
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

std::vector<PredictedVisemeCue> PredictiveVisemePlanner::plan(
    const SpeechContext& context,
    const std::vector<PredictedVisemeSpec>& specs) const {
    if (context.utterance_id == 0 || context.sample_rate == 0 || context.total_sample_count == 0) {
        throw std::invalid_argument("SpeechContext must contain utterance, rate, and duration");
    }
    if (context.total_sample_count > std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("SpeechContext duration exceeds cue sample_count range");
    }
    if (context.sample_begin > std::numeric_limits<std::uint64_t>::max() -
                                  context.total_sample_count) {
        throw std::invalid_argument("SpeechContext sample range overflows");
    }
    if (specs.empty()) {
        return {};
    }
    if (specs.size() > context.total_sample_count) {
        throw std::invalid_argument("more pronunciation units than PCM samples");
    }

    std::vector<long double> weights;
    weights.reserve(specs.size());
    long double total_weight = 0.0L;
    for (const auto& spec : specs) {
        const long double weight = std::isfinite(spec.duration_weight) && spec.duration_weight > 0.0F
            ? static_cast<long double>(spec.duration_weight)
            : 1.0L;
        weights.push_back(weight);
        total_weight += weight;
    }

    const auto total = static_cast<std::uint32_t>(context.total_sample_count);
    const auto count = specs.size();
    std::vector<std::uint32_t> durations(count, 1U);
    std::uint32_t remaining = total - static_cast<std::uint32_t>(count);
    std::vector<long double> fractions(count, 0.0L);
    std::uint32_t allocated = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const long double exact = static_cast<long double>(remaining) * weights[i] / total_weight;
        const auto whole = static_cast<std::uint32_t>(std::floor(exact));
        durations[i] += whole;
        allocated += whole;
        fractions[i] = exact - static_cast<long double>(whole);
    }
    while (allocated < remaining) {
        const auto best = static_cast<std::size_t>(std::distance(
            fractions.begin(), std::max_element(fractions.begin(), fractions.end())));
        ++durations[best];
        fractions[best] = -1.0L;
        ++allocated;
    }

    std::vector<PredictedVisemeCue> result;
    result.reserve(count);
    std::uint64_t position = context.sample_begin;
    for (std::size_t i = 0; i < count; ++i) {
        PredictedVisemeCue cue;
        cue.utterance_id = context.utterance_id;
        cue.sample_begin = position;
        cue.sample_count = durations[i];
        cue.sample_rate = context.sample_rate;
        cue.viseme.value = specs[i].value;
        cue.viseme.confidence = clamp01(specs[i].confidence);
        cue.viseme.evidence = AnimationEvidence::PredictedPronunciation;
        result.push_back(cue);
        position += durations[i];
    }
    return result;
}

std::vector<PredictedVisemeCue> PredictiveVisemePlanner::plan(
    const std::vector<PredictedVisemeSpec>& specs) const {
    if (!has_context_) {
        throw std::logic_error("PredictiveVisemePlanner has no SpeechContext");
    }
    return plan(context_, specs);
}

} // namespace speech_animation
