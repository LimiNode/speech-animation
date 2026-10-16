#include "speech_animation/live.hpp"

#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace speech_animation::integration {

class LiveAudioReactiveSession::ProducerGuard {
public:
    explicit ProducerGuard(std::atomic<std::uint32_t>& count) : count_(count) {
        count_.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ProducerGuard() { count_.fetch_sub(1, std::memory_order_release); }

private:
    std::atomic<std::uint32_t>& count_;
};

LiveAudioReactiveSession::LiveAudioReactiveSession(LiveAudioSessionConfig config)
    : config_(std::move(config)), analyzer_(config_.analyzer) {
    if (config_.queue_capacity == 0) {
        throw std::invalid_argument("live audio queue capacity must be positive");
    }
    if (config_.max_pcm_chunk_samples == 0) {
        throw std::invalid_argument("live audio PCM chunk bound must be positive");
    }
    queue_.resize(config_.queue_capacity);
}

void LiveAudioReactiveSession::reset(std::uint64_t source_id,
                                     std::uint64_t source_epoch,
                                     std::uint64_t first_sample,
                                     std::uint32_t sample_rate) {
    if (source_id == 0 || source_epoch == 0) {
        throw std::invalid_argument("live audio source and epoch must be non-zero");
    }
    auto expected = state_.load(std::memory_order_acquire);
    while (expected != LiveSessionState::Resetting &&
           !state_.compare_exchange_weak(expected, LiveSessionState::Resetting,
                                          std::memory_order_acq_rel)) {
    }
    if (expected == LiveSessionState::Resetting) {
        throw std::logic_error("live audio reset is already in progress");
    }
    std::lock_guard<std::mutex> lifecycle_lock(consumer_mutex_);
    while (producers_in_flight_.load(std::memory_order_acquire) != 0) {
        std::this_thread::yield();
    }
    for (auto& slot : queue_) slot.reset();
    write_index_.store(0, std::memory_order_relaxed);
    read_index_.store(0, std::memory_order_relaxed);
    analyzer_.reset();
    source_id_ = source_id;
    source_epoch_ = source_epoch;
    next_input_sample_ = first_sample;
    sample_rate_ = sample_rate;
    initialized_ = sample_rate != 0;
    terminal_emitted_ = false;
    muted_.store(false, std::memory_order_release);
    state_.store(LiveSessionState::Active, std::memory_order_release);
}

std::size_t LiveAudioReactiveSession::queued_chunks() const noexcept {
    const auto write = write_index_.load(std::memory_order_acquire);
    const auto read = read_index_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(write - read);
}

QueuePushResult LiveAudioReactiveSession::initialize_from_chunk(const LivePcmChunk& chunk) {
    if (chunk.source_id == 0 || chunk.source_epoch == 0 ||
        chunk.sample_rate == 0 || chunk.pcm.empty()) {
        return QueuePushResult::Invalid;
    }
    if (source_id_ != 0 && chunk.source_id != source_id_) {
        return QueuePushResult::Invalid;
    }
    if (source_epoch_ != 0 && chunk.source_epoch != source_epoch_) {
        return QueuePushResult::Invalid;
    }
    source_id_ = chunk.source_id;
    source_epoch_ = chunk.source_epoch;
    sample_rate_ = chunk.sample_rate;
    next_input_sample_ = chunk.first_sample;
    initialized_ = true;
    return QueuePushResult::Accepted;
}

QueuePushResult LiveAudioReactiveSession::push(LivePcmChunk chunk) {
    ProducerGuard guard(producers_in_flight_);
    if (state_.load(std::memory_order_acquire) != LiveSessionState::Active) {
        return QueuePushResult::RejectedTerminal;
    }
    if (chunk.source_id == 0 || chunk.source_epoch == 0 || chunk.sample_rate == 0 ||
        chunk.pcm.empty() ||
        chunk.pcm.size() > std::numeric_limits<std::uint32_t>::max()) {
        return QueuePushResult::Invalid;
    }
    if (chunk.sample_count == 0) {
        chunk.sample_count = static_cast<std::uint32_t>(chunk.pcm.size());
    }
    if (chunk.sample_count == 0 ||
        chunk.sample_count > config_.max_pcm_chunk_samples ||
        chunk.sample_count != chunk.pcm.size() ||
        chunk.first_sample > std::numeric_limits<std::uint64_t>::max() - chunk.sample_count) {
        return QueuePushResult::Invalid;
    }
    if (!initialized_) {
        if (initialize_from_chunk(chunk) != QueuePushResult::Accepted) {
            return QueuePushResult::Invalid;
        }
    }
    else if (chunk.source_id != source_id_ || chunk.source_epoch != source_epoch_ ||
             chunk.sample_rate != sample_rate_ ||
             chunk.first_sample != next_input_sample_) {
        return QueuePushResult::Invalid;
    }
    const auto write = write_index_.load(std::memory_order_relaxed);
    const auto read = read_index_.load(std::memory_order_acquire);
    if (write - read >= config_.queue_capacity) {
        return QueuePushResult::Full;
    }
    const auto slot = write % config_.queue_capacity;
    queue_[slot].emplace(QueuedChunk{std::move(chunk), muted()});
    if (state_.load(std::memory_order_acquire) != LiveSessionState::Active) {
        queue_[slot].reset();
        return QueuePushResult::RejectedTerminal;
    }
    next_input_sample_ = queue_[slot]->chunk.first_sample + queue_[slot]->chunk.sample_count;
    write_index_.store(write + 1, std::memory_order_release);
    return QueuePushResult::Accepted;
}

void LiveAudioReactiveSession::drain_one(std::vector<SpeechAnimationSpan>& output) {
    const auto read = read_index_.load(std::memory_order_relaxed);
    auto queued = std::move(*queue_[read % config_.queue_capacity]);
    queue_[read % config_.queue_capacity].reset();
    read_index_.store(read + 1, std::memory_order_release);

    std::vector<float> muted_pcm;
    const auto* samples = queued.chunk.pcm.data();
    if (queued.muted_at_capture) {
        muted_pcm.assign(queued.chunk.pcm.size(), 0.0F);
        samples = muted_pcm.data();
    }
    const PcmChunk pcm{queued.chunk.source_id, queued.chunk.first_sample,
                       queued.chunk.sample_rate, samples, queued.chunk.sample_count};
    auto spans = analyzer_.feed(pcm);
    output.insert(output.end(), spans.begin(), spans.end());
}

std::vector<SpeechAnimationSpan> LiveAudioReactiveSession::process_available() {
    std::lock_guard<std::mutex> lifecycle_lock(consumer_mutex_);
    if (state_.load(std::memory_order_acquire) == LiveSessionState::Resetting) {
        return {};
    }
    std::vector<SpeechAnimationSpan> output;
    while (true) {
        auto read = read_index_.load(std::memory_order_relaxed);
        const auto write = write_index_.load(std::memory_order_acquire);
        if (read == write) break;
        drain_one(output);
    }
    if (state_.load(std::memory_order_acquire) == LiveSessionState::Stopped &&
        !terminal_emitted_) {
        while (producers_in_flight_.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        while (read_index_.load(std::memory_order_relaxed) !=
               write_index_.load(std::memory_order_acquire)) {
            drain_one(output);
        }
        const auto tail = analyzer_.flush();
        output.insert(output.end(), tail.begin(), tail.end());
        terminal_emitted_ = true;
    }
    return output;
}

bool LiveAudioReactiveSession::stop() {
    auto expected = LiveSessionState::Active;
    return state_.compare_exchange_strong(expected, LiveSessionState::Stopped,
                                           std::memory_order_acq_rel);
}

} // namespace speech_animation::integration
