#include "speech_animation/live.hpp"

#include <cstdio>
#include <thread>
#include <stdexcept>
#include <string>
#include <vector>

using namespace speech_animation;
using namespace speech_animation::integration;

namespace {

void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                                 ": CHECK failed: " + expression);
    }
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

LivePcmChunk chunk(std::uint64_t source, std::uint64_t first,
                   std::uint32_t rate, std::size_t count, float value,
                   std::uint64_t epoch = 1) {
    LivePcmChunk result;
    result.source_id = source;
    result.source_epoch = epoch;
    result.first_sample = first;
    result.sample_rate = rate;
    result.pcm.assign(count, value);
    return result;
}

LiveAudioSessionConfig config(std::size_t capacity = 8) {
    LiveAudioSessionConfig result;
    result.queue_capacity = capacity;
    result.analyzer.output_hop_samples = 4;
    result.analyzer.energy_attack_ms = 0.0F;
    result.analyzer.energy_release_ms = 0.0F;
    result.analyzer.mouth_attack_ms = 0.0F;
    result.analyzer.mouth_release_ms = 0.0F;
    result.analyzer.normalizer_attack_ms = 0.0F;
    result.analyzer.normalizer_release_ms = 0.0F;
    return result;
}

} // namespace

int main() {
    try {
        LiveAudioReactiveSession session(config());
        CHECK(session.push(chunk(7, 0, 48000, 4, 0.8F)) == QueuePushResult::Accepted);
        CHECK(session.push(chunk(7, 4, 48000, 4, 0.0F)) == QueuePushResult::Accepted);
        CHECK(session.push(chunk(7, 8, 48000, 4, 0.8F)) == QueuePushResult::Accepted);
        const auto cues = session.process_available();
        CHECK(cues.size() == 3);
        CHECK(cues[0].sample_begin == 0 && cues[0].sample_count == 4);
        CHECK(cues[1].activity == SpeechActivity::Silence);
        CHECK(cues[0].activity == SpeechActivity::Active);
        CHECK(cues[2].activity == SpeechActivity::Active);
        CHECK(cues[0].viseme == std::nullopt);
        CHECK(cues[0].mouth_open > 0.0F);

        // The result is invariant under chunk partitioning on the capture clock.
        LiveAudioReactiveSession one(config());
        LiveAudioReactiveSession many(config());
        CHECK(one.push(chunk(8, 100, 24000, 12, 0.6F)) == QueuePushResult::Accepted);
        CHECK(many.push(chunk(8, 100, 24000, 4, 0.6F)) == QueuePushResult::Accepted);
        CHECK(many.push(chunk(8, 104, 24000, 3, 0.6F)) == QueuePushResult::Accepted);
        CHECK(many.push(chunk(8, 107, 24000, 5, 0.6F)) == QueuePushResult::Accepted);
        const auto one_cues = one.process_available();
        const auto many_cues = many.process_available();
        CHECK(one_cues.size() == many_cues.size());
        for (std::size_t index = 0; index < one_cues.size(); ++index) {
            CHECK(one_cues[index].sample_begin == many_cues[index].sample_begin);
            CHECK(one_cues[index].sample_count == many_cues[index].sample_count);
            CHECK(one_cues[index].mouth_open == many_cues[index].mouth_open);
            CHECK(one_cues[index].activity == many_cues[index].activity);
        }

        // Mute preserves the capture range while producing neutral audio cues.
        LiveAudioReactiveSession muted(config());
        muted.set_muted(true);
        CHECK(muted.push(chunk(9, 0, 48000, 4, 0.9F)) == QueuePushResult::Accepted);
        muted.set_muted(false);
        auto muted_cues = muted.process_available();
        CHECK(muted_cues.size() == 1);
        CHECK(muted_cues.front().activity == SpeechActivity::Silence);
        CHECK(muted_cues.front().mouth_open == 0.0F);
        CHECK(muted.push(chunk(9, 4, 48000, 4, 0.9F)) == QueuePushResult::Accepted);
        const auto unmuted_cues = muted.process_available();
        CHECK(unmuted_cues.size() == 1);
        CHECK(unmuted_cues.front().activity == SpeechActivity::Active);

        // Overflow, discontinuity, source fencing, stop and restart are explicit.
        LiveAudioReactiveSession bounded(config(1));
        CHECK(bounded.push(chunk(10, 0, 48000, 4, 0.5F)) == QueuePushResult::Accepted);
        CHECK(bounded.push(chunk(10, 4, 48000, 4, 0.5F)) == QueuePushResult::Full);
        CHECK(bounded.push(chunk(10, 8, 48000, 4, 0.5F)) == QueuePushResult::Invalid);
        CHECK(bounded.push(chunk(10, 4, 48000, 5000, 0.5F)) == QueuePushResult::Invalid);
        (void)bounded.process_available();
        CHECK(bounded.push(chunk(11, 4, 48000, 4, 0.5F)) == QueuePushResult::Invalid);
        CHECK(bounded.push(chunk(10, 4, 48000, 4, 0.5F)) == QueuePushResult::Accepted);
        CHECK(bounded.stop());
        CHECK(!bounded.stop());
        CHECK(bounded.push(chunk(10, 8, 48000, 4, 0.5F)) == QueuePushResult::RejectedTerminal);
        (void)bounded.process_available();
        CHECK(bounded.process_available().empty());
        bounded.reset(12, 2, 0, 16000);
        CHECK(bounded.push(chunk(12, 0, 16000, 4, 0.5F, 2)) == QueuePushResult::Accepted);
        CHECK(bounded.push(chunk(12, 3, 16000, 4, 0.5F, 2)) == QueuePushResult::Invalid);
        CHECK(bounded.push(chunk(12, 4, 16000, 4, 0.5F, 1)) == QueuePushResult::Invalid);

        // Independent sessions never share adaptive state or source identity.
        LiveAudioReactiveSession left(config());
        LiveAudioReactiveSession right(config());
        CHECK(left.push(chunk(20, 0, 48000, 4, 0.8F)) == QueuePushResult::Accepted);
        CHECK(right.push(chunk(21, 0, 48000, 4, 0.0F)) == QueuePushResult::Accepted);
        CHECK(left.process_available().front().activity == SpeechActivity::Active);
        CHECK(right.process_available().front().activity == SpeechActivity::Silence);

        // Reset and consumer drain are serialized; this must not race queue
        // slot cleanup or analyzer state.
        LiveAudioReactiveSession lifecycle(config());
        CHECK(lifecycle.push(chunk(30, 0, 48000, 4096, 0.4F)) == QueuePushResult::Accepted);
        std::thread consumer([&lifecycle]() { (void)lifecycle.process_available(); });
        lifecycle.reset(30, 2, 0, 48000);
        consumer.join();
        CHECK(lifecycle.push(chunk(30, 0, 48000, 4, 0.4F, 2)) == QueuePushResult::Accepted);
        return 0;
    }
    catch (const std::exception& error) {
        return std::fprintf(stderr, "%s\n", error.what());
    }
}
