#include <speech_animation/integration.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace speech_animation;
using namespace speech_animation::integration;

static void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                                 ": CHECK failed: " + expression);
    }
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

static SpeechTimingChunk chunk(std::uint64_t request_id,
                               std::uint64_t first_sample,
                               std::uint32_t sample_rate,
                               std::size_t count,
                               float value) {
    SpeechTimingChunk result;
    result.request_id = request_id;
    result.first_sample = first_sample;
    result.sample_rate = sample_rate;
    result.sample_count = static_cast<std::uint32_t>(count);
    result.pcm.assign(count, value);
    return result;
}

int main() {
    try {
        PipelineConfig config;
        config.queue_capacity = 1;
        config.analyzer.output_hop_samples = 16;
        config.terminal_tail_samples = 32;
        config.analyzer.energy_attack_ms = 0.0F;
        config.analyzer.energy_release_ms = 0.0F;
        config.analyzer.mouth_attack_ms = 0.0F;
        config.analyzer.mouth_release_ms = 0.0F;
        config.analyzer.normalizer_attack_ms = 0.0F;
        config.analyzer.normalizer_release_ms = 0.0F;

        SpeechAnimationPipeline pipeline(config);
        CHECK(pipeline.push(chunk(7, 0, 48000, 16, 0.8F)) == QueuePushResult::Accepted);
        CHECK(pipeline.push(chunk(7, 16, 48000, 16, 0.8F)) == QueuePushResult::Full);
        auto first = pipeline.process_available();
        CHECK(first.size() == 1);
        CHECK(first.front().output_first_sample == 0);
        CHECK(first.front().output_sample_count == 16);
        CHECK(first.front().input_first_sample == 0);
        CHECK(first.front().input_sample_count == 16);
        CHECK(first.front().activity == SpeechActivity::Active);
        CHECK(first.front().mouth_open > 0.0F);

        CHECK(pipeline.push(chunk(7, 16, 48000, 16, 0.8F)) == QueuePushResult::Accepted);
        CHECK(pipeline.push(chunk(7, 8, 48000, 16, 0.8F)) == QueuePushResult::Invalid);
        CHECK(pipeline.cancel());
        CHECK(pipeline.push(chunk(7, 32, 48000, 16, 0.8F)) == QueuePushResult::RejectedTerminal);
        auto terminal = pipeline.process_available();
        CHECK(!terminal.empty());
        CHECK(terminal.back().terminal);
        CHECK(terminal.back().terminal_state == PipelineState::Cancelled);
        CHECK(terminal.back().activity == SpeechActivity::Silence);
        CHECK(terminal.back().mouth_open == 0.0F);
        CHECK(terminal.back().kind == SpeechAnimationReceiptKind::TerminalFade);
        CHECK(terminal.back().to_json().find("\"kind\":\"terminal_fade\"") !=
              std::string::npos);

        SpeechAnimationPipeline completed(config);
        CHECK(completed.push(chunk(8, 0, 48000, 16, 0.5F)) == QueuePushResult::Accepted);
        CHECK(completed.complete());
        CHECK(!completed.complete());
        const auto completed_receipts = completed.process_available();
        CHECK(!completed_receipts.empty());
        CHECK(completed_receipts.back().terminal);
        CHECK(completed_receipts.back().terminal_state == PipelineState::Completed);

        std::uint64_t previous_end = 0;
        bool first_receipt = true;
        auto verify_range = [&](const SpeechAnimationReceipt& receipt) {
            if (!first_receipt) {
                CHECK(receipt.output_first_sample == previous_end);
            }
            previous_end = receipt.output_first_sample + receipt.output_sample_count;
            first_receipt = false;
            const auto json = receipt.to_json();
            CHECK(json.find("\"request_id\":7") != std::string::npos);
            CHECK(json.find("\"mouth_open\":") != std::string::npos);
        };
        for (const auto& receipt : first) verify_range(receipt);
        for (const auto& receipt : terminal) verify_range(receipt);
        CHECK(previous_end == 32);
        CHECK(terminal.back().output_sample_count == 0);
        CHECK(terminal.back().terminal_fade_sample_count == config.terminal_tail_samples);
        CHECK(terminal.back().output_first_sample == 32);

        // Artificial processing delays do not enter the sample-addressed result.
        PipelineConfig invariant_config = config;
        invariant_config.queue_capacity = 4;
        SpeechAnimationPipeline immediate(invariant_config);
        SpeechAnimationPipeline delayed(invariant_config);
        const auto first_delayed_chunk = chunk(9, 0, 48000, 16, 0.6F);
        const auto second_delayed_chunk = chunk(9, 16, 48000, 16, 0.2F);
        CHECK(immediate.push(first_delayed_chunk) == QueuePushResult::Accepted);
        auto immediate_receipts = immediate.process_available();
        CHECK(immediate.push(second_delayed_chunk) == QueuePushResult::Accepted);
        CHECK(immediate.complete());

        CHECK(delayed.push(first_delayed_chunk) == QueuePushResult::Accepted);
        CHECK(delayed.push(second_delayed_chunk) == QueuePushResult::Accepted);
        CHECK(delayed.complete());
        auto immediate_tail = immediate.process_available();
        auto delayed_receipts = delayed.process_available();
        immediate_receipts.insert(immediate_receipts.end(), immediate_tail.begin(), immediate_tail.end());
        CHECK(immediate_receipts.size() == delayed_receipts.size());
        for (std::size_t i = 0; i < immediate_receipts.size(); ++i) {
            CHECK(immediate_receipts[i].output_first_sample == delayed_receipts[i].output_first_sample);
            CHECK(immediate_receipts[i].output_sample_count == delayed_receipts[i].output_sample_count);
            CHECK(immediate_receipts[i].activity == delayed_receipts[i].activity);
            CHECK(immediate_receipts[i].mouth_open == delayed_receipts[i].mouth_open);
            CHECK(immediate_receipts[i].terminal_fade_sample_count ==
                  delayed_receipts[i].terminal_fade_sample_count);
        }

        SpeechAnimationPipeline no_pcm(config);
        CHECK(no_pcm.begin(99, 100, 48000) == QueuePushResult::Accepted);
        CHECK(no_pcm.cancel());
        const auto no_pcm_receipts = no_pcm.process_available();
        CHECK(no_pcm_receipts.size() == 1);
        CHECK(no_pcm_receipts.front().terminal);
        CHECK(no_pcm_receipts.front().output_first_sample == 100);
        CHECK(no_pcm_receipts.front().output_sample_count == 0);

        // Terminal reason is published atomically with terminal admission.
        for (int iteration = 0; iteration < 25; ++iteration) {
            SpeechAnimationPipeline raced(config);
            CHECK(raced.begin(200 + static_cast<std::uint64_t>(iteration),
                              0, 48000) == QueuePushResult::Accepted);
            std::thread canceller([&raced]() {
                (void)raced.cancel();
            });
            std::vector<SpeechAnimationReceipt> raced_receipts;
            while (!raced.terminal() || raced_receipts.empty()) {
                auto batch = raced.process_available();
                raced_receipts.insert(raced_receipts.end(), batch.begin(), batch.end());
                std::this_thread::yield();
            }
            auto late_cancel_batch = raced.process_available();
            raced_receipts.insert(raced_receipts.end(), late_cancel_batch.begin(), late_cancel_batch.end());
            canceller.join();
            CHECK(!raced_receipts.empty());
            CHECK(raced_receipts.back().terminal);
            CHECK(raced_receipts.back().terminal_state == PipelineState::Cancelled);
        }
        for (int iteration = 0; iteration < 25; ++iteration) {
            SpeechAnimationPipeline raced(config);
            CHECK(raced.begin(300 + static_cast<std::uint64_t>(iteration),
                              0, 48000) == QueuePushResult::Accepted);
            std::thread completer([&raced]() {
                (void)raced.complete();
            });
            std::vector<SpeechAnimationReceipt> raced_receipts;
            while (!raced.terminal() || raced_receipts.empty()) {
                auto batch = raced.process_available();
                raced_receipts.insert(raced_receipts.end(), batch.begin(), batch.end());
                std::this_thread::yield();
            }
            auto late_complete_batch = raced.process_available();
            raced_receipts.insert(raced_receipts.end(),
                                  late_complete_batch.begin(), late_complete_batch.end());
            completer.join();
            CHECK(!raced_receipts.empty());
            CHECK(raced_receipts.back().terminal);
            CHECK(raced_receipts.back().terminal_state == PipelineState::Completed);
        }

        // Exercise the actual one-producer/one-consumer queue contract.
        PipelineConfig threaded_config = config;
        threaded_config.queue_capacity = 8;
        SpeechAnimationPipeline threaded(threaded_config);
        std::vector<SpeechAnimationReceipt> threaded_receipts;
        std::atomic<bool> producer_done{false};
        std::atomic<bool> producer_failed{false};
        std::thread producer([&]() {
            for (std::uint64_t index = 0; index < 200; ++index) {
                const auto first_sample = index * 16;
                while (threaded.push(chunk(123, first_sample, 48000, 16, 0.4F)) ==
                       QueuePushResult::Full) {
                    std::this_thread::yield();
                }
            }
            if (!threaded.complete()) {
                producer_failed.store(true, std::memory_order_release);
            }
            producer_done.store(true, std::memory_order_release);
        });
        while (!producer_done.load(std::memory_order_acquire) ||
               threaded.queued_chunks() != 0 || !threaded.terminal()) {
            auto batch = threaded.process_available();
            threaded_receipts.insert(threaded_receipts.end(), batch.begin(), batch.end());
            std::this_thread::yield();
        }
        auto final_batch = threaded.process_available();
        threaded_receipts.insert(threaded_receipts.end(), final_batch.begin(), final_batch.end());
        producer.join();
        CHECK(!producer_failed.load(std::memory_order_acquire));
        CHECK(!threaded_receipts.empty());
        CHECK(threaded_receipts.back().terminal);
        CHECK(threaded_receipts.back().output_first_sample == 3200);
        CHECK(threaded_receipts.back().output_sample_count == 0);

        // Core and integration have no bridge dependency; this executable only
        // includes the neutral integration header.
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
