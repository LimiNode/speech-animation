#pragma once

#include "speech_animation/integration.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
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

// A batch of live cues carries the capture identity alongside the generic
// sample-addressed spans. Consumers must fence this identity before applying
// cues: sample positions may legitimately repeat after a source restart.
struct LiveAudioAnimationBatch {
    std::uint64_t source_id = 0;
    std::uint64_t source_epoch = 0;
    std::vector<SpeechAnimationSpan> spans;

    bool empty() const noexcept { return spans.empty(); }
    std::size_t size() const noexcept { return spans.size(); }
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

    // Drains queued chunks and returns sample-addressed AudioReactive spans
    // together with the source identity that produced them.
    // Mute is represented by zero PCM on the same capture clock, preserving
    // continuity without fabricating samples.
    LiveAudioAnimationBatch process_available();

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

    void drain_one(LiveAudioAnimationBatch& batch);
    QueuePushResult initialize_from_chunk(const LivePcmChunk& chunk);

    LiveAudioSessionConfig config_;
    SpeechAnimationAnalyzer analyzer_;
    std::vector<std::optional<QueuedChunk>> queue_;
    std::atomic<std::uint64_t> write_index_{0};
    std::atomic<std::uint64_t> read_index_{0};
    // Low 63 bits count admitted producers. The high bit closes admission
    // during reset, making reset-vs-push linearizable without blocking push().
    std::atomic<std::uint64_t> producer_admission_{0};
    std::atomic<LiveSessionState> state_{LiveSessionState::Active};
    std::atomic<bool> muted_{false};
    std::atomic<std::uint64_t> published_source_id_{0};
    std::atomic<std::uint64_t> published_source_epoch_{0};
    mutable std::mutex consumer_mutex_;
    std::uint64_t source_id_ = 0;
    std::uint64_t source_epoch_ = 0;
    std::uint64_t next_input_sample_ = 0;
    std::uint32_t sample_rate_ = 0;
    bool sample_clock_initialized_ = false;
    bool initialized_ = false;
    bool terminal_emitted_ = false;
    // Coalesced muted range awaiting output. A hop that overlaps this range
    // is forced neutral even when the range crosses an input chunk boundary.
    std::optional<std::pair<std::uint64_t, std::uint64_t>> muted_range_;
};

} // namespace speech_animation::integration
