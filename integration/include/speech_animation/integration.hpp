#pragma once

#include "speech_animation/analyzer.hpp"

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace speech_animation::integration {

// Neutral application-layer DTO. A bridge may populate this from its own
// SpeechTimingChunk without making the analyzer depend on bridge headers.
struct SpeechTimingChunk {
    std::uint64_t request_id = 0;
    std::uint64_t first_sample = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t sample_count = 0; // zero means derive from pcm.size()
    std::vector<float> pcm;
};

enum class QueuePushResult {
    Accepted,
    Full,
    RejectedTerminal,
    Invalid,
};

enum class PipelineState {
    Active,
    Completed,
    Cancelled,
};

enum class SpeechAnimationReceiptKind {
    AudioSpan,
    TerminalFade,
};

// One JSON-serializable record for an audio span or terminal fade cue.
struct SpeechAnimationReceipt {
    SpeechAnimationReceiptKind kind = SpeechAnimationReceiptKind::AudioSpan;
    std::uint64_t request_id = 0;
    std::uint64_t input_first_sample = 0;
    std::uint32_t input_sample_count = 0;
    std::uint64_t output_first_sample = 0;
    std::uint32_t output_sample_count = 0;
    std::uint32_t sample_rate = 0;
    SpeechActivity activity = SpeechActivity::Silence;
    float mouth_open = 0.0F;
    bool terminal = false;
    PipelineState terminal_state = PipelineState::Active;
    std::uint32_t terminal_fade_sample_count = 0;

    std::string to_json() const;
};

struct PipelineConfig {
    AnalyzerConfig analyzer;

    // The producer never waits for analysis. Queue slots are preallocated at
    // construction. When full, push() returns Full and the caller must retry
    // or apply its own upstream backpressure; no audio is silently dropped.
    std::size_t queue_capacity = 8;

    // Terminal fade duration anchored after the last actual PCM sample. Zero
    // means derive 160 ms from the input sample rate; a non-zero value is exact.
    float terminal_tail_ms = 160.0F;
    std::uint32_t terminal_tail_samples = 0;
};

class SpeechAnimationPipeline {
public:
    explicit SpeechAnimationPipeline(PipelineConfig config = {});

    SpeechAnimationPipeline(const SpeechAnimationPipeline&) = delete;
    SpeechAnimationPipeline& operator=(const SpeechAnimationPipeline&) = delete;

    // Establishes request metadata before any PCM arrives. Call this before
    // starting the producer when a request may complete/cancel before audio.
    QueuePushResult begin(std::uint64_t request_id,
                          std::uint64_t first_sample,
                          std::uint32_t sample_rate);

    // Non-blocking bounded producer operation. The vector is moved into the
    // queue, so callers should provide an owned PCM buffer.
    QueuePushResult push(SpeechTimingChunk chunk);

    // Consumer-side operation. It drains currently queued chunks and performs
    // all analyzer work outside the producer/audio callback.
    std::vector<SpeechAnimationReceipt> process_available();

    // Preserve bridge lifecycle semantics. Already queued audio is processed
    // before the terminal fade and exactly one terminal receipt is emitted.
    bool complete();
    bool cancel();

    bool terminal() const noexcept {
        return state_.load(std::memory_order_acquire) != PipelineState::Active;
    }
    PipelineState state() const noexcept { return state_.load(std::memory_order_acquire); }
    std::size_t queued_chunks() const noexcept;
    const PipelineConfig& config() const noexcept { return config_; }

private:
    std::vector<SpeechAnimationReceipt> process_chunk(const SpeechTimingChunk& chunk);
    std::vector<SpeechAnimationReceipt> emit_terminal_fade();
    std::uint32_t terminal_fade_sample_count(std::uint32_t sample_rate) const;
    std::vector<SpeechAnimationReceipt> receipts_for_spans(
        std::uint64_t request_id,
        std::uint32_t sample_rate,
        const std::vector<SpeechAnimationSpan>& spans) const;
    QueuePushResult begin_unlocked(std::uint64_t request_id,
                                   std::uint64_t first_sample,
                                   std::uint32_t sample_rate);

    PipelineConfig config_;
    SpeechAnimationAnalyzer analyzer_;
    std::vector<std::optional<SpeechTimingChunk>> queue_;
    std::atomic<std::uint64_t> write_index_{0};
    std::atomic<std::uint64_t> read_index_{0};
    std::atomic<std::uint32_t> producers_in_flight_{0};
    std::uint64_t request_id_ = 0;
    bool request_initialized_ = false;
    bool terminal_emitted_ = false;
    std::atomic<PipelineState> state_{PipelineState::Active};
    std::uint32_t sample_rate_ = 0;
    bool input_position_initialized_ = false;
    std::uint64_t next_input_sample_ = 0;
    std::atomic<std::uint64_t> published_request_id_{0};
    std::atomic<std::uint32_t> published_sample_rate_{0};
    std::atomic<std::uint64_t> published_next_sample_{0};
    std::uint64_t consumer_request_id_ = 0;
    std::uint32_t consumer_sample_rate_ = 0;
};

} // namespace speech_animation::integration
