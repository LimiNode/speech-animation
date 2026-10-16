#pragma once

#include "speech_animation/integration.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace speech_animation::integration {

// Neutral capture-side DTO. A microphone, decoder, or network jitter buffer
// owns the source-specific callback and converts it to this sample-addressed
// chunk before entering the session.
struct LivePcmChunk {
    std::uint64_t source_id = 0;
    std::uint64_t source_epoch = 0;
    std::uint64_t first_sample = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t sample_count = 0; // zero means derive from pcm.size()
    std::vector<float> pcm;
};

enum class LiveSessionState {
    Active,
    Stopped,
    Resetting,
};

struct LiveAudioSessionConfig {
    AnalyzerConfig analyzer;
    std::size_t queue_capacity = 8;
    std::uint32_t max_pcm_chunk_samples = 4096;
};

// A continuous, audio-only capture session. It has no text, pronunciation,
// playback-device, or renderer dependency. The producer operation is bounded
// and non-blocking; only the consumer calls the analyzer.
class LiveAudioReactiveSession final {
public:
    explicit LiveAudioReactiveSession(LiveAudioSessionConfig config = {});

    LiveAudioReactiveSession(const LiveAudioReactiveSession&) = delete;
    LiveAudioReactiveSession& operator=(const LiveAudioReactiveSession&) = delete;

    // Starts/restarts a capture epoch. source_epoch must be unique for every
    // restart, even when source_id and first_sample are reused. This is
    // lifecycle-owner controlled and must not be called by a delayed callback.
    void reset(std::uint64_t source_id,
               std::uint64_t source_epoch,
               std::uint64_t first_sample = 0,
               std::uint32_t sample_rate = 0);

    // Moves an owned chunk into the bounded ring. Full means the newest chunk
    // was rejected; no capture thread waits for a consumer.
    QueuePushResult push(LivePcmChunk chunk);

    // Drains queued chunks and returns sample-addressed AudioReactive spans.
    // Mute is represented by zero PCM on the same capture clock, preserving
    // continuity without fabricating samples.
    std::vector<SpeechAnimationSpan> process_available();

    // Stop is terminal for the current source. process_available() drains
    // accepted PCM and flushes the final partial hop exactly once.
    bool stop();

    // The value is snapshotted when push() admits a chunk.
    void set_muted(bool muted) noexcept { muted_.store(muted, std::memory_order_release); }
    bool muted() const noexcept { return muted_.load(std::memory_order_acquire); }
    LiveSessionState state() const noexcept { return state_.load(std::memory_order_acquire); }
    std::size_t queued_chunks() const noexcept;
    const LiveAudioSessionConfig& config() const noexcept { return config_; }

private:
    class ProducerGuard;
    struct QueuedChunk {
        LivePcmChunk chunk;
        bool muted_at_capture = false;
    };

    void drain_one(std::vector<SpeechAnimationSpan>& output);
    QueuePushResult initialize_from_chunk(const LivePcmChunk& chunk);

    LiveAudioSessionConfig config_;
    SpeechAnimationAnalyzer analyzer_;
    std::vector<std::optional<QueuedChunk>> queue_;
    std::atomic<std::uint64_t> write_index_{0};
    std::atomic<std::uint64_t> read_index_{0};
    std::atomic<std::uint32_t> producers_in_flight_{0};
    std::atomic<LiveSessionState> state_{LiveSessionState::Active};
    std::atomic<bool> muted_{false};
    mutable std::mutex consumer_mutex_;
    std::uint64_t source_id_ = 0;
    std::uint64_t source_epoch_ = 0;
    std::uint64_t next_input_sample_ = 0;
    std::uint32_t sample_rate_ = 0;
    bool initialized_ = false;
    bool terminal_emitted_ = false;
};

} // namespace speech_animation::integration
