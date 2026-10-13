#pragma once

#include "speech_animation/integration.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace speech_animation::integration {

/// A snapshot of the physical playback position on an utterance sample clock.
/// The producer of the snapshot owns the epoch and request identity.
struct PlaybackCursorSnapshot {
    std::uint64_t playback_epoch = 0;
    std::uint64_t request_id = 0;
    std::uint64_t utterance_sample = 0;
    std::uint32_t sample_rate = 0;
    bool position_available = false;
};

enum class PlaybackSelectionStatus {
    Selected,
    TerminalFade,
    CursorUnavailable,
    StaleEpoch,
    StaleRequest,
    BeforeFirstCue,
    AfterLastCue,
    TerminalComplete,
};

/// The presentation result for one playback tick.
///
/// A result that is not `Selected` or `TerminalFade` is deliberately neutral:
/// presentation must not interpolate across an unavailable cursor, epoch
/// change, cancellation, or a range that has not been produced yet.
struct PlaybackAnimationSelection {
    PlaybackSelectionStatus status = PlaybackSelectionStatus::CursorUnavailable;
    std::uint64_t playback_epoch = 0;
    std::uint64_t request_id = 0;
    std::uint64_t cursor_sample = 0;
    std::uint64_t cue_first_sample = 0;
    std::uint32_t cue_sample_count = 0;
    std::uint32_t sample_rate = 0;
    SpeechActivity activity = SpeechActivity::Silence;
    float mouth_open = 0.0F;
    bool terminal = false;
    float terminal_fade_progress = 0.0F;
    // Number of cue ranges crossed since the previously selected cue. On the
    // first selection this is the number of ranges before the cursor.
    std::size_t skipped_cue_count = 0;

    std::string to_json() const;
};

/// Selects sample-addressed animation cues using the physical playback cursor.
///
/// The selector is single-consumer and intentionally does not observe callback
/// wall-clock time. `reset()` invalidates all cues from the previous playback
/// epoch immediately. `append()` accepts the ordered receipts emitted by
/// `SpeechAnimationPipeline`; malformed or out-of-order receipts are rejected.
class PlaybackLockedCueSelector final {
public:
    /// Creates a selector with a bounded cue history.
    ///
    /// The history keeps the currently selected cue and any producer-ahead
    /// cues. Once the playback cursor advances, older ranges are retired.
    /// If the producer gets more than `max_cue_count` ranges ahead while the
    /// cursor is unavailable, `append()` fails closed instead of growing
    /// without a bound.
    explicit PlaybackLockedCueSelector(std::size_t max_cue_count = 4096);

    PlaybackLockedCueSelector(const PlaybackLockedCueSelector&) = delete;
    PlaybackLockedCueSelector& operator=(const PlaybackLockedCueSelector&) = delete;

    /// Starts a new playback epoch and discards all previous cues.
    void reset(std::uint64_t playback_epoch, std::uint64_t request_id);

    /// Appends one ordered audio-span or terminal-fade receipt.
    bool append(const SpeechAnimationReceipt& receipt);

    /// Selects a neutral or active presentation state for one cursor snapshot.
    PlaybackAnimationSelection select(const PlaybackCursorSnapshot& snapshot) const;

    std::size_t cue_count() const noexcept { return cues_.size(); }
    std::size_t max_cue_count() const noexcept { return max_cue_count_; }
    bool terminal_received() const noexcept { return terminal_received_; }

private:
    std::uint64_t playback_epoch_ = 0;
    std::uint64_t request_id_ = 0;
    std::uint32_t sample_rate_ = 0;
    std::uint64_t next_sample_ = 0;
    bool initialized_ = false;
    bool terminal_received_ = false;
    std::size_t max_cue_count_ = 4096;
    mutable std::deque<SpeechAnimationReceipt> cues_;
    mutable std::optional<std::size_t> last_selected_cue_index_;

    void retire_before(std::size_t index) const;
    bool make_room_for_append();
};

} // namespace speech_animation::integration
