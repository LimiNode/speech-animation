#pragma once

#include "speech_animation/analyzer.hpp"

#include <cstddef>
#include <cstdint>
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

// One JSON-serializable record for an emitted analyzer span.
struct SpeechAnimationReceipt {
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
    bool synthetic_terminal_tail = false;

    std::string to_json() const;
};

struct PipelineConfig {
    AnalyzerConfig analyzer;

    // The producer never waits for analysis. Queue slots are preallocated at
    // construction. When full, push() returns Full and the caller must retry
    // or apply its own upstream backpressure; no audio is silently dropped.
    std::size_t queue_capacity = 8;

    // A terminal zero PCM tail is part of the same sample timeline. Zero means
    // derive 160 ms from the input sample rate; a non-zero value is exact.
    float terminal_tail_ms = 160.0F;
    std::uint32_t terminal_tail_samples = 0;
};

class SpeechAnimationPipeline {
public:
    explicit SpeechAnimationPipeline(PipelineConfig config = {});

    SpeechAnimationPipeline(const SpeechAnimationPipeline&) = delete;
    SpeechAnimationPipeline& operator=(const SpeechAnimationPipeline&) = delete;

    // Non-blocking bounded producer operation. The vector is moved into the
    // queue, so callers should provide an owned PCM buffer.
    QueuePushResult push(SpeechTimingChunk chunk);

    // Consumer-side operation. It drains currently queued chunks and performs
    // all analyzer work outside the producer/audio callback.
    std::vector<SpeechAnimationReceipt> process_available();

    // Preserve bridge lifecycle semantics. Already queued audio is processed
    // before the terminal tail and exactly one terminal receipt is emitted.
    bool complete();
    bool cancel();

    bool terminal() const noexcept { return state_ != PipelineState::Active; }
    PipelineState state() const noexcept { return state_; }
    std::size_t queued_chunks() const noexcept { return queue_size_; }
    const PipelineConfig& config() const noexcept { return config_; }

private:
    std::vector<SpeechAnimationReceipt> process_chunk(const SpeechTimingChunk& chunk,
                                                      bool synthetic_tail);
    std::vector<SpeechAnimationReceipt> emit_terminal_tail();
    std::uint32_t terminal_tail_count(std::uint32_t sample_rate) const;
    std::vector<SpeechAnimationReceipt> receipts_for_spans(
        const SpeechTimingChunk& input,
        const std::vector<SpeechAnimationSpan>& spans,
        bool synthetic_tail) const;

    PipelineConfig config_;
    SpeechAnimationAnalyzer analyzer_;
    std::vector<std::optional<SpeechTimingChunk>> queue_;
    std::size_t queue_head_ = 0;
    std::size_t queue_size_ = 0;
    std::uint64_t request_id_ = 0;
    bool request_initialized_ = false;
    bool terminal_requested_ = false;
    bool terminal_emitted_ = false;
    PipelineState state_ = PipelineState::Active;
    std::uint32_t sample_rate_ = 0;
    bool input_position_initialized_ = false;
    std::uint64_t next_input_sample_ = 0;
};

} // namespace speech_animation::integration
