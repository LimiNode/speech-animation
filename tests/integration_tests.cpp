#include <speech_animation/integration.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
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
        CHECK(terminal.back().synthetic_terminal_tail);
        CHECK(terminal.back().activity == SpeechActivity::Silence);
        CHECK(terminal.back().mouth_open == 0.0F);

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
        CHECK(previous_end == 64);
        CHECK(terminal.back().output_first_sample + terminal.back().output_sample_count == 64);

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
        }

        // Core and integration have no bridge dependency; this executable only
        // includes the neutral integration header.
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
