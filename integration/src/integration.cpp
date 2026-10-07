#include "speech_animation/integration.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
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

} // namespace

std::string SpeechAnimationReceipt::to_json() const {
    std::ostringstream json;
    json << std::setprecision(9)
         << "{\"request_id\":" << request_id
         << ",\"input_first_sample\":" << input_first_sample
         << ",\"input_sample_count\":" << input_sample_count
         << ",\"output_first_sample\":" << output_first_sample
         << ",\"output_sample_count\":" << output_sample_count
         << ",\"sample_rate\":" << sample_rate
         << ",\"activity\":\"" << activity_name(activity) << '\"'
         << ",\"mouth_open\":" << mouth_open
         << ",\"terminal\":" << (terminal ? "true" : "false")
         << ",\"terminal_state\":\"" << state_name(terminal_state) << '\"'
         << ",\"synthetic_terminal_tail\":"
         << (synthetic_terminal_tail ? "true" : "false") << '}';
    return json.str();
}

SpeechAnimationPipeline::SpeechAnimationPipeline(PipelineConfig config)
    : config_(std::move(config)), analyzer_(config_.analyzer) {
    if (config_.queue_capacity == 0 || !std::isfinite(config_.terminal_tail_ms) ||
        config_.terminal_tail_ms <= 0.0F) {
        throw std::invalid_argument("invalid speech animation pipeline configuration");
    }
    queue_.resize(config_.queue_capacity);
}

QueuePushResult SpeechAnimationPipeline::push(SpeechTimingChunk chunk) {
    if (terminal_requested_ || terminal()) {
        return QueuePushResult::RejectedTerminal;
    }
    if (chunk.request_id == 0 || chunk.sample_rate == 0 || chunk.pcm.empty() ||
        chunk.pcm.size() > std::numeric_limits<std::uint32_t>::max()) {
        return QueuePushResult::Invalid;
    }
    if (chunk.sample_count == 0) {
        chunk.sample_count = static_cast<std::uint32_t>(chunk.pcm.size());
    }
    if (chunk.sample_count != chunk.pcm.size()) {
        return QueuePushResult::Invalid;
    }
    if (chunk.first_sample > std::numeric_limits<std::uint64_t>::max() - chunk.sample_count) {
        return QueuePushResult::Invalid;
    }
    if (request_initialized_ && chunk.request_id != request_id_) {
        return QueuePushResult::Invalid;
    }
    if (input_position_initialized_ && chunk.first_sample != next_input_sample_) {
        return QueuePushResult::Invalid;
    }
    if (queue_size_ >= config_.queue_capacity) {
        return QueuePushResult::Full;
    }
    if (!request_initialized_) {
        request_initialized_ = true;
        request_id_ = chunk.request_id;
        sample_rate_ = chunk.sample_rate;
    } else if (chunk.sample_rate != sample_rate_) {
        return QueuePushResult::Invalid;
    }
    if (!input_position_initialized_) {
        input_position_initialized_ = true;
    }
    next_input_sample_ = chunk.first_sample + chunk.sample_count;
    const auto slot = (queue_head_ + queue_size_) % config_.queue_capacity;
    queue_[slot].emplace(std::move(chunk));
    ++queue_size_;
    return QueuePushResult::Accepted;
}

std::vector<SpeechAnimationReceipt> SpeechAnimationPipeline::process_available() {
    std::vector<SpeechAnimationReceipt> receipts;
    while (queue_size_ != 0) {
        SpeechTimingChunk chunk = std::move(*queue_[queue_head_]);
        queue_[queue_head_].reset();
        queue_head_ = (queue_head_ + 1) % config_.queue_capacity;
        --queue_size_;
        auto chunk_receipts = process_chunk(chunk, false);
        receipts.insert(receipts.end(), chunk_receipts.begin(), chunk_receipts.end());
    }
    if (terminal_requested_ && !terminal_emitted_) {
        auto tail_receipts = emit_terminal_tail();
        receipts.insert(receipts.end(), tail_receipts.begin(), tail_receipts.end());
    }
    return receipts;
}

bool SpeechAnimationPipeline::complete() {
    if (terminal_requested_ || terminal()) {
        return false;
    }
    terminal_requested_ = true;
    state_ = PipelineState::Completed;
    return true;
}

bool SpeechAnimationPipeline::cancel() {
    if (terminal_requested_ || terminal()) {
        return false;
    }
    terminal_requested_ = true;
    state_ = PipelineState::Cancelled;
    return true;
}

std::vector<SpeechAnimationReceipt> SpeechAnimationPipeline::process_chunk(
    const SpeechTimingChunk& chunk, bool synthetic_tail) {
    const PcmChunk pcm{
        chunk.request_id,
        chunk.first_sample,
        chunk.sample_rate,
        chunk.pcm.data(),
        chunk.sample_count,
    };
    const auto spans = analyzer_.feed(pcm);
    return receipts_for_spans(chunk, spans, synthetic_tail);
}

std::vector<SpeechAnimationReceipt> SpeechAnimationPipeline::emit_terminal_tail() {
    if (!request_initialized_ || !analyzer_.initialized()) {
        terminal_emitted_ = true;
        return {};
    }

    const auto tail_count = terminal_tail_count(sample_rate_);
    SpeechTimingChunk tail;
    tail.request_id = request_id_;
    tail.first_sample = analyzer_.next_sample_position();
    tail.sample_rate = sample_rate_;
    tail.sample_count = tail_count;
    tail.pcm.assign(tail_count, 0.0F);
    auto receipts = process_chunk(tail, true);
    auto final_span = analyzer_.flush();
    auto flushed = receipts_for_spans(tail, final_span, true);
    receipts.insert(receipts.end(), flushed.begin(), flushed.end());
    if (!receipts.empty()) {
        receipts.back().terminal = true;
        receipts.back().terminal_state = state_;
    }
    terminal_emitted_ = true;
    return receipts;
}

std::uint32_t SpeechAnimationPipeline::terminal_tail_count(std::uint32_t sample_rate) const {
    if (config_.terminal_tail_samples != 0) {
        return config_.terminal_tail_samples;
    }
    // The caller normally passes zero because analyzer keeps the authoritative
    // sample rate internally; the common bridge sample rate is supplied by the
    // first chunk in process_available().
    const auto rate = sample_rate == 0 ? 48000U : sample_rate;
    const auto requested = static_cast<double>(config_.terminal_tail_ms) * 0.001 * rate;
    if (requested > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::invalid_argument("terminal tail is too large for the sample rate");
    }
    return std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::llround(requested)));
}

std::vector<SpeechAnimationReceipt> SpeechAnimationPipeline::receipts_for_spans(
    const SpeechTimingChunk& input,
    const std::vector<SpeechAnimationSpan>& spans,
    bool synthetic_tail) const {
    std::vector<SpeechAnimationReceipt> receipts;
    receipts.reserve(spans.size());
    for (const auto& span : spans) {
        SpeechAnimationReceipt receipt;
        receipt.request_id = input.request_id;
        receipt.input_first_sample = input.first_sample;
        receipt.input_sample_count = input.sample_count;
        receipt.output_first_sample = span.sample_begin;
        receipt.output_sample_count = span.sample_count;
        receipt.sample_rate = span.sample_rate;
        receipt.activity = span.activity;
        receipt.mouth_open = span.mouth_open;
        receipt.synthetic_terminal_tail = synthetic_tail;
        receipts.push_back(receipt);
    }
    return receipts;
}

} // namespace speech_animation::integration
