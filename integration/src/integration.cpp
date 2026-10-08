#include "speech_animation/integration.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace speech_animation::integration {
namespace {

const char* activity_name(SpeechActivity activity) noexcept {
    return activity == SpeechActivity::Active ? "active" : "silence";
}

const char* state_name(PipelineState state) noexcept {
    switch (state) {
    case PipelineState::Active: return "active";
    case PipelineState::Completed: return "completed";
    case PipelineState::Cancelled: return "cancelled";
    }
    return "active";
}

const char* kind_name(SpeechAnimationReceiptKind kind) noexcept {
    return kind == SpeechAnimationReceiptKind::AudioSpan ? "audio_span" : "terminal_fade";
}

class ProducerGuard {
public:
    explicit ProducerGuard(std::atomic<std::uint32_t>& count) : count_(count) {
        count_.fetch_add(1, std::memory_order_acq_rel);
    }
    ~ProducerGuard() {
        count_.fetch_sub(1, std::memory_order_release);
    }

private:
    std::atomic<std::uint32_t>& count_;
};

} // namespace

std::string SpeechAnimationReceipt::to_json() const {
    std::ostringstream json;
    json << std::setprecision(9)
         << "{\"kind\":\"" << kind_name(kind) << '\"'
         << ",\"request_id\":" << request_id
         << ",\"input_first_sample\":" << input_first_sample
         << ",\"input_sample_count\":" << input_sample_count
         << ",\"output_first_sample\":" << output_first_sample
         << ",\"output_sample_count\":" << output_sample_count
         << ",\"sample_rate\":" << sample_rate
         << ",\"activity\":\"" << activity_name(activity) << '"'
         << ",\"mouth_open\":" << mouth_open
         << ",\"terminal\":" << (terminal ? "true" : "false")
         << ",\"terminal_state\":\"" << state_name(terminal_state) << '"'
         << ",\"terminal_fade_sample_count\":" << terminal_fade_sample_count << '}';
    return json.str();
}

SpeechAnimationPipeline::SpeechAnimationPipeline(PipelineConfig config)
    : config_(std::move(config)), analyzer_(config_.analyzer) {
    if (config_.queue_capacity == 0 || !std::isfinite(config_.terminal_tail_ms) ||
        config_.terminal_tail_ms <= 0.0F) {
        throw std::invalid_argument("invalid speech animation pipeline configuration");
    }
    // Slots are allocated before the producer can call push().
    queue_.resize(config_.queue_capacity);
}

std::size_t SpeechAnimationPipeline::queued_chunks() const noexcept {
    const auto write = write_index_.load(std::memory_order_acquire);
    const auto read = read_index_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(write - read);
}

QueuePushResult SpeechAnimationPipeline::begin_unlocked(std::uint64_t request_id,
                                                        std::uint64_t first_sample,
                                                        std::uint32_t sample_rate) {
    if (terminal()) {
        return QueuePushResult::RejectedTerminal;
    }
    if (request_id == 0 || sample_rate == 0 || request_initialized_) {
        return QueuePushResult::Invalid;
    }
    request_initialized_ = true;
    request_id_ = request_id;
    sample_rate_ = sample_rate;
    input_position_initialized_ = true;
    next_input_sample_ = first_sample;
    published_request_id_.store(request_id, std::memory_order_release);
    published_sample_rate_.store(sample_rate, std::memory_order_release);
    published_next_sample_.store(first_sample, std::memory_order_release);
    return QueuePushResult::Accepted;
}

QueuePushResult SpeechAnimationPipeline::begin(std::uint64_t request_id,
                                               std::uint64_t first_sample,
                                               std::uint32_t sample_rate) {
    ProducerGuard producer_guard(producers_in_flight_);
    return begin_unlocked(request_id, first_sample, sample_rate);
}

QueuePushResult SpeechAnimationPipeline::push(SpeechTimingChunk chunk) {
    ProducerGuard producer_guard(producers_in_flight_);
    if (terminal()) {
        return QueuePushResult::RejectedTerminal;
    }
    if (chunk.request_id == 0 || chunk.sample_rate == 0 || chunk.pcm.empty() ||
        chunk.pcm.size() > std::numeric_limits<std::uint32_t>::max()) {
        return QueuePushResult::Invalid;
    }
    if (chunk.sample_count == 0) {
        chunk.sample_count = static_cast<std::uint32_t>(chunk.pcm.size());
    }
    if (chunk.sample_count != chunk.pcm.size() ||
        chunk.first_sample > std::numeric_limits<std::uint64_t>::max() - chunk.sample_count) {
        return QueuePushResult::Invalid;
    }
    if (request_initialized_ && chunk.request_id != request_id_) {
        return QueuePushResult::Invalid;
    }
    if (input_position_initialized_ && chunk.first_sample != next_input_sample_) {
        return QueuePushResult::Invalid;
    }

    const auto write = write_index_.load(std::memory_order_relaxed);
    const auto read = read_index_.load(std::memory_order_acquire);
    if (write - read >= config_.queue_capacity) {
        return QueuePushResult::Full;
    }
    if (!request_initialized_) {
        const auto result = begin_unlocked(chunk.request_id,
                                           chunk.first_sample,
                                           chunk.sample_rate);
        if (result != QueuePushResult::Accepted) {
            return result;
        }
    } else if (chunk.sample_rate != sample_rate_) {
        return QueuePushResult::Invalid;
    }

    const auto slot = write % config_.queue_capacity;
    queue_[slot].emplace(std::move(chunk));
    // A cancellation racing this commit may linearize before the chunk. Do not
    // publish the slot in that case; the consumer can never observe it.
    if (terminal()) {
        queue_[slot].reset();
        return QueuePushResult::RejectedTerminal;
    }
    // The moved-from local is no longer usable, so read metadata from the slot.
    const auto& accepted = *queue_[slot];
    next_input_sample_ = accepted.first_sample + accepted.sample_count;
    input_position_initialized_ = true;
    published_next_sample_.store(next_input_sample_, std::memory_order_release);
    write_index_.store(write + 1, std::memory_order_release);
    return QueuePushResult::Accepted;
}

std::vector<SpeechAnimationReceipt> SpeechAnimationPipeline::process_available() {
    std::vector<SpeechAnimationReceipt> receipts;
    while (true) {
        auto read = read_index_.load(std::memory_order_relaxed);
        const auto write = write_index_.load(std::memory_order_acquire);
        if (read == write) {
            if (!terminal()) {
                break;
            }
            // Observe a producer commit that raced the first empty check.
            if (read == write_index_.load(std::memory_order_acquire)) {
                break;
            }
            continue;
        }
        SpeechTimingChunk chunk = std::move(*queue_[read % config_.queue_capacity]);
        queue_[read % config_.queue_capacity].reset();
        read_index_.store(read + 1, std::memory_order_release);
        auto chunk_receipts = process_chunk(chunk);
        receipts.insert(receipts.end(), chunk_receipts.begin(), chunk_receipts.end());
    }
    if (terminal() && !terminal_emitted_) {
        // A push that started before cancellation either publishes a slot or
        // observes the terminal state before this point. Waiting is confined
        // to the consumer thread; the audio producer never waits.
        while (producers_in_flight_.load(std::memory_order_acquire) != 0) {
            std::this_thread::yield();
        }
        // Drain a final producer commit that completed during the first pass.
        while (true) {
            auto read = read_index_.load(std::memory_order_relaxed);
            const auto write = write_index_.load(std::memory_order_acquire);
            if (read == write) {
                break;
            }
            SpeechTimingChunk chunk = std::move(*queue_[read % config_.queue_capacity]);
            queue_[read % config_.queue_capacity].reset();
            read_index_.store(read + 1, std::memory_order_release);
            auto chunk_receipts = process_chunk(chunk);
            receipts.insert(receipts.end(), chunk_receipts.begin(), chunk_receipts.end());
        }
        auto terminal_receipts = emit_terminal_fade();
        receipts.insert(receipts.end(), terminal_receipts.begin(), terminal_receipts.end());
    }
    return receipts;
}

bool SpeechAnimationPipeline::complete() {
    auto expected = PipelineState::Active;
    return state_.compare_exchange_strong(expected, PipelineState::Completed,
                                          std::memory_order_acq_rel);
}

bool SpeechAnimationPipeline::cancel() {
    auto expected = PipelineState::Active;
    return state_.compare_exchange_strong(expected, PipelineState::Cancelled,
                                          std::memory_order_acq_rel);
}

std::vector<SpeechAnimationReceipt> SpeechAnimationPipeline::process_chunk(
    const SpeechTimingChunk& chunk) {
    const PcmChunk pcm{
        chunk.request_id,
        chunk.first_sample,
        chunk.sample_rate,
        chunk.pcm.data(),
        chunk.sample_count,
    };
    const auto spans = analyzer_.feed(pcm);
    if (consumer_request_id_ == 0) {
        consumer_request_id_ = chunk.request_id;
        consumer_sample_rate_ = chunk.sample_rate;
    }
    return receipts_for_spans(chunk.request_id, chunk.sample_rate, spans);
}

std::vector<SpeechAnimationReceipt> SpeechAnimationPipeline::emit_terminal_fade() {
    const auto request_id = consumer_request_id_ != 0
        ? consumer_request_id_
        : published_request_id_.load(std::memory_order_acquire);
    const auto sample_rate = consumer_sample_rate_ != 0
        ? consumer_sample_rate_
        : published_sample_rate_.load(std::memory_order_acquire);
    const auto anchor = analyzer_.initialized()
        ? analyzer_.next_sample_position()
        : published_next_sample_.load(std::memory_order_acquire);

    std::vector<SpeechAnimationReceipt> receipts;
    if (request_id == 0 || sample_rate == 0) {
        terminal_emitted_ = true;
        return receipts;
    }
    if (analyzer_.initialized()) {
        const auto final_spans = analyzer_.flush();
        auto audio_receipts = receipts_for_spans(request_id, sample_rate, final_spans);
        receipts.insert(receipts.end(), audio_receipts.begin(), audio_receipts.end());
    }

    SpeechAnimationReceipt fade;
    fade.kind = SpeechAnimationReceiptKind::TerminalFade;
    fade.request_id = request_id;
    fade.input_first_sample = anchor;
    fade.input_sample_count = 0;
    fade.output_first_sample = anchor;
    fade.output_sample_count = 0;
    fade.sample_rate = sample_rate;
    fade.activity = SpeechActivity::Silence;
    fade.mouth_open = 0.0F;
    fade.terminal = true;
    fade.terminal_state = state();
    fade.terminal_fade_sample_count = terminal_fade_sample_count(sample_rate);
    receipts.push_back(fade);
    terminal_emitted_ = true;
    return receipts;
}

std::uint32_t SpeechAnimationPipeline::terminal_fade_sample_count(std::uint32_t sample_rate) const {
    if (config_.terminal_tail_samples != 0) {
        return config_.terminal_tail_samples;
    }
    const auto rate = sample_rate == 0 ? 48000U : sample_rate;
    const auto requested = static_cast<double>(config_.terminal_tail_ms) * 0.001 * rate;
    if (requested > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument("terminal tail is too large for the sample rate");
    }
    return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::llround(requested)));
}

std::vector<SpeechAnimationReceipt> SpeechAnimationPipeline::receipts_for_spans(
    std::uint64_t request_id,
    std::uint32_t sample_rate,
    const std::vector<SpeechAnimationSpan>& spans) const {
    std::vector<SpeechAnimationReceipt> receipts;
    receipts.reserve(spans.size());
    for (const auto& span : spans) {
        SpeechAnimationReceipt receipt;
        receipt.kind = SpeechAnimationReceiptKind::AudioSpan;
        receipt.request_id = request_id;
        // Provenance is the contributing canonical PCM range, not the callback
        // chunk that happened to make the hop complete.
        receipt.input_first_sample = span.sample_begin;
        receipt.input_sample_count = span.sample_count;
        receipt.output_first_sample = span.sample_begin;
        receipt.output_sample_count = span.sample_count;
        receipt.sample_rate = sample_rate;
        receipt.activity = span.activity;
        receipt.mouth_open = span.mouth_open;
        receipts.push_back(receipt);
    }
    return receipts;
}

} // namespace speech_animation::integration
