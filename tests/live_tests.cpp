#include "speech_animation/live.hpp"

#include <atomic>
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
        CHECK(cues.source_id == 7 && cues.source_epoch == 1);
        CHECK(cues.size() == 3);
        CHECK(cues.spans[0].sample_begin == 0 && cues.spans[0].sample_count == 4);
        CHECK(cues.spans[1].activity == SpeechActivity::Silence);
        CHECK(cues.spans[0].activity == SpeechActivity::Active);
        CHECK(cues.spans[2].activity == SpeechActivity::Active);
        CHECK(cues.spans[0].viseme == std::nullopt);
        CHECK(cues.spans[0].mouth_open > 0.0F);

        // The result is invariant under chunk partitioning on the capture clock.
        LiveAudioReactiveSession one(config());
        LiveAudioReactiveSession many(config());
        CHECK(one.push(chunk(8, 100, 24000, 12, 0.6F)) == QueuePushResult::Accepted);
        CHECK(many.push(chunk(8, 100, 24000, 4, 0.6F)) == QueuePushResult::Accepted);
        CHECK(many.push(chunk(8, 104, 24000, 3, 0.6F)) == QueuePushResult::Accepted);
        CHECK(many.push(chunk(8, 107, 24000, 5, 0.6F)) == QueuePushResult::Accepted);
        const auto one_cues = one.process_available();
        const auto many_cues = many.process_available();
        CHECK(one_cues.source_id == 8 && one_cues.source_epoch == 1);
        CHECK(many_cues.source_id == 8 && many_cues.source_epoch == 1);
        CHECK(one_cues.size() == many_cues.size());
        for (std::size_t index = 0; index < one_cues.size(); ++index) {
            CHECK(one_cues.spans[index].sample_begin == many_cues.spans[index].sample_begin);
            CHECK(one_cues.spans[index].sample_count == many_cues.spans[index].sample_count);
            CHECK(one_cues.spans[index].mouth_open == many_cues.spans[index].mouth_open);
            CHECK(one_cues.spans[index].activity == many_cues.spans[index].activity);
        }

        // Mute preserves the capture range while producing neutral audio cues,
        // even when the analyzer has a non-zero release time and retained an
        // active pre-mute state.
        auto muted_config = config();
        muted_config.analyzer.mouth_release_ms = 200.0F;
        muted_config.analyzer.energy_release_ms = 200.0F;
        LiveAudioReactiveSession muted(muted_config);
        CHECK(muted.push(chunk(9, 0, 48000, 4, 0.9F)) == QueuePushResult::Accepted);
        const auto pre_mute_cues = muted.process_available();
        CHECK(pre_mute_cues.size() == 1);
        CHECK(pre_mute_cues.spans.front().mouth_open > 0.0F);
        muted.set_muted(true);
        CHECK(muted.push(chunk(9, 4, 48000, 4, 0.9F)) == QueuePushResult::Accepted);
        muted.set_muted(false);
        auto muted_cues = muted.process_available();
        CHECK(muted_cues.size() == 1);
        CHECK(muted_cues.source_id == 9 && muted_cues.source_epoch == 1);
        CHECK(muted_cues.spans.front().activity == SpeechActivity::Silence);
        CHECK(muted_cues.spans.front().mouth_open == 0.0F);
        CHECK(muted_cues.spans.front().energy == 0.0F);
        CHECK(muted.push(chunk(9, 8, 48000, 4, 0.9F)) == QueuePushResult::Accepted);
        const auto unmuted_cues = muted.process_available();
        CHECK(unmuted_cues.size() == 1);
        CHECK(unmuted_cues.spans.front().activity == SpeechActivity::Active);

        // The same neutral contract applies to a muted partial hop flushed by
        // stop(), not only to completed hops.
        auto tail_config = muted_config;
        tail_config.analyzer.output_hop_samples = 8;
        LiveAudioReactiveSession muted_tail(tail_config);
        CHECK(muted_tail.push(chunk(9, 0, 48000, 8, 0.9F)) == QueuePushResult::Accepted);
        CHECK(muted_tail.process_available().size() == 1);
        muted_tail.set_muted(true);
        CHECK(muted_tail.push(chunk(9, 8, 48000, 4, 0.9F)) == QueuePushResult::Accepted);
        muted_tail.set_muted(false);
        CHECK(muted_tail.stop());
        const auto muted_tail_cues = muted_tail.process_available();
        CHECK(muted_tail_cues.size() == 1);
        CHECK(muted_tail_cues.spans.front().activity == SpeechActivity::Silence);
        CHECK(muted_tail_cues.spans.front().mouth_open == 0.0F);

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
        CHECK(left.process_available().spans.front().activity == SpeechActivity::Active);
        CHECK(right.process_available().spans.front().activity == SpeechActivity::Silence);

        // Reset and consumer drain are serialized; this must not race queue
        // slot cleanup or analyzer state.
        LiveAudioReactiveSession lifecycle(config());
        CHECK(lifecycle.push(chunk(30, 0, 48000, 4096, 0.4F)) == QueuePushResult::Accepted);
        std::thread consumer([&lifecycle]() { (void)lifecycle.process_available(); });
        lifecycle.reset(30, 2, 0, 48000);
        consumer.join();
        CHECK(lifecycle.push(chunk(30, 0, 48000, 4, 0.4F, 2)) == QueuePushResult::Accepted);

        // A reset-provided sample clock remains authoritative even when the
        // sample rate is learned from the first PCM chunk.
        LiveAudioReactiveSession fenced(config());
        fenced.reset(40, 7, 100, 0);
        CHECK(fenced.push(chunk(40, 99, 16000, 4, 0.4F, 7)) == QueuePushResult::Invalid);
        CHECK(fenced.push(chunk(40, 100, 16000, 4, 0.4F, 7)) == QueuePushResult::Accepted);
        CHECK(fenced.process_available().source_epoch == 7);
        fenced.stop();
        const auto stopped = fenced.process_available();
        CHECK(stopped.source_id == 40 && stopped.source_epoch == 7);

        // Resetting to a new epoch changes the envelope even when the sample
        // coordinates and source ID are reused.
        fenced.reset(40, 8, 100, 16000);
        CHECK(fenced.push(chunk(40, 100, 16000, 4, 0.4F, 8)) == QueuePushResult::Accepted);
        const auto restarted = fenced.process_available();
        CHECK(restarted.source_id == 40 && restarted.source_epoch == 8);

        // A first process call may race the first producer publication. The
        // non-empty batch identity must come from the queued chunk itself,
        // never from a stale pre-publication session snapshot.
        for (int iteration = 0; iteration < 100; ++iteration) {
            LiveAudioReactiveSession raced(config());
            std::atomic<bool> start{false};
            LiveAudioAnimationBatch first_batch;
            std::thread consumer_thread([&]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                first_batch = raced.process_available();
            });
            start.store(true, std::memory_order_release);
            CHECK(raced.push(chunk(50, 0, 48000, 4, 0.5F, 3)) ==
                  QueuePushResult::Accepted);
            consumer_thread.join();
            const auto second_batch = raced.process_available();
            CHECK((first_batch.source_id == 0 && first_batch.source_epoch == 0) ||
                  (first_batch.source_id == 50 && first_batch.source_epoch == 3));
            CHECK((second_batch.source_id == 0 && second_batch.source_epoch == 0) ||
                  (second_batch.source_id == 50 && second_batch.source_epoch == 3));
            if (!first_batch.empty()) {
                CHECK(first_batch.source_id == 50 && first_batch.source_epoch == 3);
            }
            if (!second_batch.empty()) {
                CHECK(second_batch.source_id == 50 && second_batch.source_epoch == 3);
            }
        }

        // Reset admission and producer admission have a single linearization
        // gate. Either the push is admitted before reset and then drained, or
        // it is rejected; neither path may touch reset-owned queue storage.
        for (int iteration = 0; iteration < 100; ++iteration) {
            LiveAudioReactiveSession concurrent(config());
            std::atomic<bool> start{false};
            std::thread producer([&]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                (void)concurrent.push(chunk(60, 0, 48000, 4, 0.5F, 1));
            });
            std::thread resetter([&]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                concurrent.reset(60, 2, 0, 48000);
            });
            start.store(true, std::memory_order_release);
            producer.join();
            resetter.join();
            CHECK(concurrent.push(chunk(60, 0, 48000, 4, 0.5F, 2)) ==
                  QueuePushResult::Accepted);
            const auto post_reset = concurrent.process_available();
            CHECK(post_reset.source_id == 60 && post_reset.source_epoch == 2);
        }

        // queued_chunks() is observational during reset but must always stay
        // bounded; reset index publication must never expose unsigned wrap.
        for (int iteration = 0; iteration < 100; ++iteration) {
            LiveAudioReactiveSession observed(config(4));
            CHECK(observed.push(chunk(65, 0, 48000, 4, 0.5F, 1)) ==
                  QueuePushResult::Accepted);
            std::atomic<bool> start{false};
            std::atomic<bool> finished{false};
            std::thread resetter([&]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                observed.reset(65, 2, 0, 48000);
                finished.store(true, std::memory_order_release);
            });
            start.store(true, std::memory_order_release);
            while (!finished.load(std::memory_order_acquire)) {
                CHECK(observed.queued_chunks() <= observed.config().queue_capacity);
                std::this_thread::yield();
            }
            resetter.join();
            CHECK(observed.queued_chunks() == 0);
        }

        // Analyzer sample-rate conflicts are rejected before queue admission.
        auto rate_config = config();
        rate_config.analyzer.sample_rate = 48000;
        LiveAudioReactiveSession rate_checked(rate_config);
        bool reset_rate_rejected = false;
        try {
            rate_checked.reset(70, 1, 0, 16000);
        }
        catch (const std::invalid_argument&) {
            reset_rate_rejected = true;
        }
        CHECK(reset_rate_rejected);
        rate_checked.reset(70, 1, 0, 0);
        CHECK(rate_checked.push(chunk(70, 0, 16000, 4, 0.5F, 1)) ==
              QueuePushResult::Invalid);
        return 0;
    }
    catch (const std::exception& error) {
        return std::fprintf(stderr, "%s\n", error.what());
    }
}
