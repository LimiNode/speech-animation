#include "speech_animation/playback.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace speech_animation::integration {
namespace {

const char* status_name(PlaybackSelectionStatus status) noexcept {
    switch (status) {
    case PlaybackSelectionStatus::Selected: return "selected";
    case PlaybackSelectionStatus::TerminalFade: return "terminal_fade";
    case PlaybackSelectionStatus::CursorUnavailable: return "cursor_unavailable";
    case PlaybackSelectionStatus::StaleEpoch: return "stale_epoch";
    case PlaybackSelectionStatus::StaleRequest: return "stale_request";
    case PlaybackSelectionStatus::BeforeFirstCue: return "before_first_cue";
    case PlaybackSelectionStatus::AfterLastCue: return "after_last_cue";
    case PlaybackSelectionStatus::TerminalComplete: return "terminal_complete";
    }
    return "cursor_unavailable";
}

const char* viseme_name(Viseme value) noexcept {
    switch (value) {
    case Viseme::Neutral: return "neutral";
    case Viseme::Closed: return "closed";
    case Viseme::A: return "a";
    case Viseme::E: return "e";
    case Viseme::I: return "i";
    case Viseme::O: return "o";
    case Viseme::U: return "u";
    case Viseme::FV: return "fv";
    }
    return "neutral";
}

const char* evidence_name(AnimationEvidence value) noexcept {
    switch (value) {
    case AnimationEvidence::AudioReactive: return "audio_reactive";
    case AnimationEvidence::PredictedPronunciation: return "predicted_pronunciation";
    case AnimationEvidence::AcousticAligned: return "acoustic_aligned";
    }
    return "audio_reactive";
}

} // namespace

PlaybackLockedCueSelector::PlaybackLockedCueSelector(std::size_t max_cue_count)
    : max_cue_count_(max_cue_count) {
    if (max_cue_count_ == 0) {
        throw std::invalid_argument("playback cue history bound must be greater than zero");
    }
}

void PlaybackLockedCueSelector::retire_before(std::size_t index) const {
    const auto retired = std::min(index, cues_.size());
    for (std::size_t count = 0; count < retired; ++count) {
        cues_.pop_front();
    }
    if (last_selected_cue_index_.has_value()) {
        if (last_selected_cue_index_.value() < retired) {
            last_selected_cue_index_ = 0;
        }
        else {
            last_selected_cue_index_ = last_selected_cue_index_.value() - retired;
        }
    }
}

bool PlaybackLockedCueSelector::make_room_for_append() {
    if (cues_.size() < max_cue_count_) {
        return true;
    }
    if (last_selected_cue_index_.has_value() &&
        last_selected_cue_index_.value() != 0) {
        retire_before(last_selected_cue_index_.value());
    }
    return cues_.size() < max_cue_count_;
}

std::string PlaybackAnimationSelection::to_json() const {
    std::ostringstream json;
    json << std::setprecision(9)
         << "{\"status\":\"" << status_name(status) << '"'
         << ",\"playback_epoch\":" << playback_epoch
         << ",\"request_id\":" << request_id
         << ",\"cursor_sample\":" << cursor_sample
         << ",\"cue_first_sample\":" << cue_first_sample
         << ",\"cue_sample_count\":" << cue_sample_count
         << ",\"sample_rate\":" << sample_rate
         << ",\"activity\":\""
         << (activity == SpeechActivity::Active ? "active" : "silence") << '"'
         << ",\"mouth_open\":" << mouth_open
         << ",\"viseme\":";
    if (viseme.has_value()) {
        json << "{\"value\":\"" << viseme_name(viseme->value)
             << "\",\"confidence\":" << viseme->confidence
             << ",\"evidence\":\"" << evidence_name(viseme->evidence)
             << "\"}";
    }
    else {
        json << "null";
    }
    json
         << ",\"terminal\":" << (terminal ? "true" : "false")
         << ",\"terminal_fade_progress\":" << terminal_fade_progress
         << ",\"skipped_cue_count\":" << skipped_cue_count << '}';
    return json.str();
}

void PlaybackLockedCueSelector::reset(
    std::uint64_t playback_epoch,
    std::uint64_t request_id) {
    playback_epoch_ = playback_epoch;
    request_id_ = request_id;
    sample_rate_ = 0;
    next_sample_ = 0;
    initialized_ = false;
    terminal_received_ = false;
    cues_.clear();
    predictions_.clear();
    next_prediction_sample_ = 0;
    last_selected_cue_index_.reset();
}

bool PlaybackLockedCueSelector::append(const SpeechAnimationReceipt& receipt) {
    if (terminal_received_ || receipt.request_id == 0 || request_id_ == 0 ||
        receipt.request_id != request_id_ || receipt.sample_rate == 0) {
        return false;
    }
    if (receipt.kind == SpeechAnimationReceiptKind::AudioSpan) {
        if (receipt.terminal || receipt.output_sample_count == 0 ||
            (sample_rate_ != 0 && receipt.sample_rate != sample_rate_) ||
            (receipt.output_first_sample != next_sample_ && initialized_) ||
            receipt.output_first_sample >
                std::numeric_limits<std::uint64_t>::max() - receipt.output_sample_count) {
            return false;
        }
    if (!initialized_) {
        next_sample_ = receipt.output_first_sample;
        initialized_ = true;
    }
    if (!make_room_for_append()) {
        return false;
    }
    sample_rate_ = receipt.sample_rate;
    next_sample_ = receipt.output_first_sample + receipt.output_sample_count;
    cues_.push_back(receipt);
        return true;
    }

    if (!receipt.terminal || receipt.output_sample_count != 0 ||
        receipt.terminal_fade_sample_count == 0 ||
        (initialized_ && receipt.sample_rate != sample_rate_) ||
        (receipt.input_first_sample != next_sample_ && initialized_) ||
        receipt.input_first_sample >
            std::numeric_limits<std::uint64_t>::max() -
                receipt.terminal_fade_sample_count) {
        return false;
    }
    if (!initialized_) {
        next_sample_ = receipt.input_first_sample;
        initialized_ = true;
    }
    if (!make_room_for_append()) {
        return false;
    }
    sample_rate_ = receipt.sample_rate;
    next_sample_ = receipt.input_first_sample;
    terminal_received_ = true;
    cues_.push_back(receipt);
    return true;
}

bool PlaybackLockedCueSelector::append_prediction(
    std::uint64_t playback_epoch,
    std::uint64_t request_id,
    std::uint32_t sample_rate,
    const PredictedVisemeCue& cue) {
    if (terminal_received_ || playback_epoch != playback_epoch_ ||
        request_id == 0 || request_id != request_id_ ||
        sample_rate == 0 || cue.sample_count == 0 ||
        cue.sample_begin > std::numeric_limits<std::uint64_t>::max() - cue.sample_count ||
        cue.estimate.evidence != AnimationEvidence::PredictedPronunciation ||
        !std::isfinite(cue.estimate.confidence) || cue.estimate.confidence < 0.0F ||
        cue.estimate.confidence > 1.0F) {
        return false;
    }
    if (!predictions_.empty() && cue.sample_begin != next_prediction_sample_) {
        return false;
    }
    if (sample_rate_ != 0 && sample_rate != sample_rate_) {
        return false;
    }
    if (predictions_.size() >= max_cue_count_) {
        return false;
    }
    predictions_.push_back(cue);
    next_prediction_sample_ = cue.sample_begin + cue.sample_count;
    if (sample_rate_ == 0) {
        sample_rate_ = sample_rate;
    }
    return true;
}

PlaybackAnimationSelection PlaybackLockedCueSelector::select(
    const PlaybackCursorSnapshot& snapshot) const {
    PlaybackAnimationSelection result;
    result.playback_epoch = snapshot.playback_epoch;
    result.request_id = snapshot.request_id;
    result.cursor_sample = snapshot.utterance_sample;
    result.sample_rate = snapshot.sample_rate;
    if (!snapshot.position_available) {
        result.status = PlaybackSelectionStatus::CursorUnavailable;
        return result;
    }
    if (snapshot.playback_epoch != playback_epoch_) {
        result.status = PlaybackSelectionStatus::StaleEpoch;
        return result;
    }
    if (snapshot.request_id != request_id_) {
        result.status = PlaybackSelectionStatus::StaleRequest;
        return result;
    }
    if (sample_rate_ != 0 && snapshot.sample_rate != sample_rate_) {
        result.status = PlaybackSelectionStatus::CursorUnavailable;
        return result;
    }
    while (predictions_.size() > 1 &&
           snapshot.utterance_sample >=
               predictions_.front().sample_begin + predictions_.front().sample_count) {
        predictions_.pop_front();
    }
    std::size_t skipped = 0;
    for (std::size_t index = 0; index < cues_.size(); ++index) {
        const auto& cue = cues_[index];
        const auto skipped_since_previous = [&]() {
            if (!last_selected_cue_index_.has_value() ||
                index <= last_selected_cue_index_.value() + 1) {
                return last_selected_cue_index_.has_value() ? std::size_t{0} : index;
            }
            return index - last_selected_cue_index_.value() - 1;
        }();
        if (cue.kind == SpeechAnimationReceiptKind::TerminalFade) {
            const auto fade_end = cue.input_first_sample + cue.terminal_fade_sample_count;
            if (snapshot.utterance_sample < cue.input_first_sample) {
                break;
            }
            if (snapshot.utterance_sample < fade_end) {
                result.status = PlaybackSelectionStatus::TerminalFade;
                result.cue_first_sample = cue.input_first_sample;
                result.sample_rate = cue.sample_rate;
                result.terminal = true;
                result.terminal_fade_progress = std::clamp(
                    static_cast<float>(snapshot.utterance_sample - cue.input_first_sample) /
                        static_cast<float>(cue.terminal_fade_sample_count),
                    0.0F,
                    1.0F);
                result.skipped_cue_count = skipped_since_previous;
                retire_before(index);
                last_selected_cue_index_ = 0;
                return result;
            }
            result.status = PlaybackSelectionStatus::TerminalComplete;
            result.terminal = true;
            result.skipped_cue_count = skipped_since_previous;
            last_selected_cue_index_ = index;
            return result;
        }
        const auto cue_end = cue.output_first_sample + cue.output_sample_count;
        if (snapshot.utterance_sample < cue.output_first_sample) {
            result.status = skipped == 0
                ? PlaybackSelectionStatus::BeforeFirstCue
                : PlaybackSelectionStatus::AfterLastCue;
            result.skipped_cue_count = skipped_since_previous;
            return result;
        }
        if (snapshot.utterance_sample < cue_end) {
            result.status = PlaybackSelectionStatus::Selected;
            result.cue_first_sample = cue.output_first_sample;
            result.cue_sample_count = cue.output_sample_count;
            result.sample_rate = cue.sample_rate;
            result.activity = cue.activity;
            result.mouth_open = cue.mouth_open;
            result.terminal = false;
            for (const auto& prediction : predictions_) {
                const auto prediction_end = prediction.sample_begin + prediction.sample_count;
                if (snapshot.utterance_sample >= prediction.sample_begin &&
                    snapshot.utterance_sample < prediction_end) {
                    result.viseme = prediction.estimate;
                    break;
                }
            }
            result.skipped_cue_count = skipped_since_previous;
            retire_before(index);
            last_selected_cue_index_ = 0;
            return result;
        }
        ++skipped;
    }
    result.status = skipped == 0
        ? PlaybackSelectionStatus::BeforeFirstCue
        : PlaybackSelectionStatus::AfterLastCue;
    result.skipped_cue_count = skipped;
    return result;
}

} // namespace speech_animation::integration
