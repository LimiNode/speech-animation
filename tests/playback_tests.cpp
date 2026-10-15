#include "speech_animation/playback.hpp"

#include <cassert>
#include <stdexcept>

using namespace speech_animation::integration;

namespace {

SpeechAnimationReceipt audio(
    std::uint64_t request_id,
    std::uint64_t first_sample,
    std::uint32_t sample_count,
    float mouth_open) {
    SpeechAnimationReceipt receipt;
    receipt.kind = SpeechAnimationReceiptKind::AudioSpan;
    receipt.request_id = request_id;
    receipt.output_first_sample = first_sample;
    receipt.output_sample_count = sample_count;
    receipt.sample_rate = 1000;
    receipt.activity = speech_animation::SpeechActivity::Active;
    receipt.mouth_open = mouth_open;
    return receipt;
}

SpeechAnimationReceipt fade(std::uint64_t request_id, std::uint64_t anchor) {
    SpeechAnimationReceipt receipt;
    receipt.kind = SpeechAnimationReceiptKind::TerminalFade;
    receipt.request_id = request_id;
    receipt.input_first_sample = anchor;
    receipt.sample_rate = 1000;
    receipt.terminal = true;
    receipt.terminal_fade_sample_count = 100;
    return receipt;
}

speech_animation::PredictedVisemeCue predicted(std::uint64_t first_sample,
                             std::uint32_t sample_count,
                             speech_animation::Viseme value,
                             float confidence = 0.8F) {
    speech_animation::PredictedVisemeCue cue;
    cue.sample_begin = first_sample;
    cue.sample_count = sample_count;
    cue.estimate = speech_animation::VisemeEstimate{
        value, confidence, speech_animation::AnimationEvidence::PredictedPronunciation};
    return cue;
}

PlaybackCursorSnapshot cursor(
    std::uint64_t epoch,
    std::uint64_t request_id,
    std::uint64_t sample) {
    return PlaybackCursorSnapshot{epoch, request_id, sample, 1000, true};
}

} // namespace

int main() {
    PlaybackLockedCueSelector selector;
    selector.reset(7, 42);
    assert(selector.append(audio(42, 100, 100, 0.25F)));
    assert(selector.append(audio(42, 200, 100, 0.75F)));
    assert(selector.append(fade(42, 300)));

    // Predictive cues overlay the audio-reactive timeline without changing
    // its sample ranges; uncovered ranges retain the audio fallback.
    selector.reset(7, 42);
    assert(selector.append(audio(42, 100, 100, 0.25F)));
    assert(selector.append(audio(42, 200, 100, 0.75F)));
    assert(selector.append_prediction(42, 1000, predicted(100, 100, speech_animation::Viseme::A)));
    assert(selector.append_prediction(42, 1000, predicted(200, 100, speech_animation::Viseme::O)));
    const auto predicted_first = selector.select(cursor(7, 42, 150));
    assert(predicted_first.viseme.has_value());
    assert(predicted_first.viseme->value == speech_animation::Viseme::A);
    assert(predicted_first.viseme->evidence ==
           speech_animation::AnimationEvidence::PredictedPronunciation);
    selector.reset(7, 42);
    assert(selector.append(audio(42, 100, 100, 0.25F)));
    const auto fallback = selector.select(cursor(7, 42, 150));
    assert(!fallback.viseme.has_value());
    assert(fallback.mouth_open == 0.25F);

    selector.reset(7, 42);
    assert(!selector.append_prediction(41, 1000, predicted(100, 100, speech_animation::Viseme::A)));
    assert(selector.append_prediction(42, 1000, predicted(100, 100, speech_animation::Viseme::A)));
    assert(!selector.append_prediction(42, 1000, predicted(250, 100, speech_animation::Viseme::O)));
    assert(selector.append(audio(42, 100, 100, 0.25F)));
    assert(selector.append(fade(42, 200)));
    assert(!selector.append_prediction(42, 1000, predicted(200, 100, speech_animation::Viseme::O)));
    assert(selector.select(cursor(7, 42, 150)).to_json().find("\"viseme\"") !=
           std::string::npos);

    selector.reset(7, 42);
    assert(selector.append(audio(42, 100, 100, 0.25F)));
    assert(selector.append(audio(42, 200, 100, 0.75F)));
    assert(selector.append(fade(42, 300)));
    const auto first = selector.select(cursor(7, 42, 150));
    assert(first.status == PlaybackSelectionStatus::Selected);
    assert(first.mouth_open == 0.25F);
    assert(first.skipped_cue_count == 0);

    const auto second = selector.select(cursor(7, 42, 250));
    assert(second.status == PlaybackSelectionStatus::Selected);
    assert(second.mouth_open == 0.75F);
    assert(second.skipped_cue_count == 0);

    selector.reset(7, 42);
    assert(selector.append(audio(42, 100, 100, 0.25F)));
    assert(selector.append(audio(42, 200, 100, 0.75F)));
    assert(selector.append(audio(42, 300, 100, 0.5F)));
    assert(selector.select(cursor(7, 42, 150)).skipped_cue_count == 0);
    assert(selector.select(cursor(7, 42, 350)).skipped_cue_count == 1);
    selector.reset(7, 42);
    assert(selector.append(audio(42, 100, 100, 0.25F)));
    assert(selector.append(audio(42, 200, 100, 0.75F)));
    assert(selector.append(fade(42, 300)));

    const auto fading = selector.select(cursor(7, 42, 350));
    assert(fading.status == PlaybackSelectionStatus::TerminalFade);
    assert(fading.terminal);
    assert(fading.terminal_fade_progress == 0.5F);
    assert(fading.mouth_open == 0.0F);

    const auto unavailable = selector.select(
        PlaybackCursorSnapshot{7, 42, 200, 1000, false});
    assert(unavailable.status == PlaybackSelectionStatus::CursorUnavailable);
    assert(unavailable.mouth_open == 0.0F);

    const auto stale = selector.select(cursor(6, 42, 150));
    assert(stale.status == PlaybackSelectionStatus::StaleEpoch);
    selector.reset(8, 43);
    assert(selector.select(cursor(7, 42, 150)).status ==
        PlaybackSelectionStatus::StaleEpoch);
    assert(!selector.append(audio(42, 100, 100, 1.0F)));

    PlaybackLockedCueSelector bounded(2);
    bounded.reset(9, 99);
    assert(bounded.append(audio(99, 0, 100, 0.1F)));
    assert(bounded.append(audio(99, 100, 100, 0.2F)));
    assert(!bounded.append(audio(99, 200, 100, 0.3F)));
    assert(bounded.cue_count() == 2);
    assert(bounded.select(cursor(9, 99, 50)).mouth_open == 0.1F);
    assert(bounded.select(cursor(9, 99, 150)).mouth_open == 0.2F);
    assert(bounded.cue_count() == 1);
    assert(bounded.append(audio(99, 200, 100, 0.3F)));
    assert(bounded.cue_count() == 2);

    bool rejected_zero_bound = false;
    try {
        PlaybackLockedCueSelector invalid(0);
    }
    catch (const std::invalid_argument&) {
        rejected_zero_bound = true;
    }
    assert(rejected_zero_bound);
    return 0;
}
