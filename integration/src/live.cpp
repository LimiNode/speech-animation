#include "speech_animation/live.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace speech_animation::integration {

namespace {

constexpr std::uint64_t kResetAdmissionBit = std::uint64_t{1} << 63;
constexpr std::uint64_t kProducerCountMask = ~kResetAdmissionBit;

} // namespace

class LiveAudioReactiveSession::ProducerGuard {
public:
    explicit ProducerGuard(std::atomic<std::uint64_t>& admission)
        : admission_(admission) {
        auto snapshot = admission_.load(std::memory_order_acquire);
        while ((snapshot & kResetAdmissionBit) == 0) {
            if (admission_.compare_exchange_weak(
                    snapshot, snapshot + 1, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                admitted_ = true;
                return;
            }
        }
    }
    ~ProducerGuard() {
        if (admitted_) {
            admission_.fetch_sub(1, std::memory_order_release);
        }
    }

    bool admitted() const noexcept { return admitted_; }

private:
    std::atomic<std::uint64_t>& admission_;
    bool admitted_ = false;
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
    if (sample_rate != 0 && config_.analyzer.sample_rate != 0 &&
        sample_rate != config_.analyzer.sample_rate) {
        throw std::invalid_argument("live audio reset sample rate conflicts with analyzer config");
    }
    auto admission = producer_admission_.load(std::memory_order_acquire);
    while ((admission & kResetAdmissionBit) == 0 &&
           !producer_admission_.compare_exchange_weak(
               admission, admission | kResetAdmissionBit,
               std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
    if ((admission & kResetAdmissionBit) != 0) {
        throw std::logic_error("live audio reset is already in progress");
    }
    state_.store(LiveSessionState::Resetting, std::memory_order_release);
    std::lock_guard<std::mutex> lifecycle_lock(consumer_mutex_);
    while ((producer_admission_.load(std::memory_order_acquire) &
            kProducerCountMask) != 0) {
        std::this_thread::yield();
    }
    for (auto& slot : queue_) slot.reset();
    read_index_.store(0, std::memory_order_relaxed);
    // Publish write last: an observer that acquires the reset write index also
    // observes the reset read index, preventing a transient unsigned wrap.
    write_index_.store(0, std::memory_order_release);
    analyzer_.reset();
    consumer_source_id_ = source_id;
    consumer_source_epoch_ = source_epoch;
    source_id_ = source_id;
    source_epoch_ = source_epoch;
    next_input_sample_ = first_sample;
    sample_rate_ = sample_rate;
    sample_clock_initialized_ = true;
    initialized_ = sample_rate != 0;
    terminal_emitted_ = false;
    muted_range_.reset();
    muted_.store(false, std::memory_order_release);
    state_.store(LiveSessionState::Active, std::memory_order_release);
    producer_admission_.fetch_and(kProducerCountMask, std::memory_order_release);
}

std::size_t LiveAudioReactiveSession::queued_chunks() const noexcept {
    if (state_.load(std::memory_order_acquire) == LiveSessionState::Resetting) {
        return 0;
    }
    const auto write = write_index_.load(std::memory_order_acquire);
    const auto read = read_index_.load(std::memory_order_acquire);
    if (state_.load(std::memory_order_acquire) == LiveSessionState::Resetting ||
        write < read) {
        return 0;
    }
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
    if (sample_rate_ != 0 && chunk.sample_rate != sample_rate_) {
        return QueuePushResult::Invalid;
    }
    if (sample_clock_initialized_ && chunk.first_sample != next_input_sample_) {
        return QueuePushResult::Invalid;
    }
    source_id_ = chunk.source_id;
    source_epoch_ = chunk.source_epoch;
    sample_rate_ = chunk.sample_rate;
    if (!sample_clock_initialized_) {
        next_input_sample_ = chunk.first_sample;
        sample_clock_initialized_ = true;
    }
    initialized_ = true;
    return QueuePushResult::Accepted;
}

QueuePushResult LiveAudioReactiveSession::push(LivePcmChunk chunk) {
    ProducerGuard guard(producer_admission_);
    if (!guard.admitted()) {
        return QueuePushResult::RejectedTerminal;
    }
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
    if (config_.analyzer.sample_rate != 0 &&
        chunk.sample_rate != config_.analyzer.sample_rate) {
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

void LiveAudioReactiveSession::drain_one(LiveAudioAnimationBatch& batch) {
    const auto read = read_index_.load(std::memory_order_relaxed);
    auto queued = std::move(*queue_[read % config_.queue_capacity]);
    queue_[read % config_.queue_capacity].reset();
    read_index_.store(read + 1, std::memory_order_release);

    // The queued chunk is the publication boundary for capture identity.
    // Never derive a non-empty batch from producer-owned session fields.
    if (batch.spans.empty()) {
        batch.source_id = queued.chunk.source_id;
        batch.source_epoch = queued.chunk.source_epoch;
    }
    consumer_source_id_ = queued.chunk.source_id;
    consumer_source_epoch_ = queued.chunk.source_epoch;

    std::vector<float> muted_pcm;
    const auto* samples = queued.chunk.pcm.data();
    if (queued.muted_at_capture) {
        muted_pcm.assign(queued.chunk.pcm.size(), 0.0F);
        samples = muted_pcm.data();
    }
    const PcmChunk pcm{queued.chunk.source_id, queued.chunk.first_sample,
                       queued.chunk.sample_rate, samples, queued.chunk.sample_count};
    if (queued.muted_at_capture) {
        const auto begin = queued.chunk.first_sample;
        const auto end = begin + queued.chunk.sample_count;
        if (!muted_range_) {
            muted_range_ = std::make_pair(begin, end);
        } else {
            muted_range_->second = std::max(muted_range_->second, end);
        }
    }
    auto spans = analyzer_.feed(pcm);
    for (auto& span : spans) {
        const auto span_end = span.sample_begin + span.sample_count;
        const bool overlaps_muted = muted_range_ &&
            span.sample_begin < muted_range_->second &&
            muted_range_->first < span_end;
        if (overlaps_muted) {
            // Mute is an explicit presentation contract, not merely an audio
            // substitution. Release smoothing may retain pre-mute state, so
            // a muted range must never expose a stale mouth cue. The pending
            // range also covers hops that cross a chunk boundary.
            span.energy = 0.0F;
            span.mouth_open = 0.0F;
            span.activity = SpeechActivity::Silence;
            span.viseme.reset();
        }
        if (muted_range_ && span_end >= muted_range_->second) {
            muted_range_.reset();
        }
    }
    batch.spans.insert(batch.spans.end(), spans.begin(), spans.end());
}

LiveAudioAnimationBatch LiveAudioReactiveSession::process_available() {
    std::lock_guard<std::mutex> lifecycle_lock(consumer_mutex_);
    LiveAudioAnimationBatch batch;
    batch.source_id = consumer_source_id_;
    batch.source_epoch = consumer_source_epoch_;
    if (state_.load(std::memory_order_acquire) == LiveSessionState::Resetting) {
        return batch;
    }
    while (true) {
        auto read = read_index_.load(std::memory_order_relaxed);
        const auto write = write_index_.load(std::memory_order_acquire);
        if (read == write) break;
        drain_one(batch);
    }
    if (state_.load(std::memory_order_acquire) == LiveSessionState::Stopped &&
        !terminal_emitted_) {
        while ((producer_admission_.load(std::memory_order_acquire) &
                kProducerCountMask) != 0) {
            std::this_thread::yield();
        }
        while (read_index_.load(std::memory_order_relaxed) !=
               write_index_.load(std::memory_order_acquire)) {
            drain_one(batch);
        }
        const auto tail = analyzer_.flush();
        for (auto span : tail) {
            const auto span_end = span.sample_begin + span.sample_count;
            const bool overlaps_muted = muted_range_ &&
                span.sample_begin < muted_range_->second &&
                muted_range_->first < span_end;
            if (overlaps_muted) {
                span.energy = 0.0F;
                span.mouth_open = 0.0F;
                span.activity = SpeechActivity::Silence;
                span.viseme.reset();
            }
            batch.spans.push_back(std::move(span));
        }
        muted_range_.reset();
        terminal_emitted_ = true;
    }
    return batch;
}

bool LiveAudioReactiveSession::stop() {
    auto expected = LiveSessionState::Active;
    return state_.compare_exchange_strong(expected, LiveSessionState::Stopped,
                                           std::memory_order_acq_rel);
}

} // namespace speech_animation::integration
