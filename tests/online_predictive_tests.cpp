#include <speech_animation/predictive.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace speech_animation;

static void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error("CHECK failed at line " + std::to_string(line) +
                                 ": " + expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

static std::vector<PredictedVisemeSpec> sequence() {
    return {
        {{Viseme::Closed, 0.9F, AnimationEvidence::PredictedPronunciation}, 1.0F},
        {{Viseme::A, 0.8F, AnimationEvidence::PredictedPronunciation}, 2.0F},
        {{Viseme::FV, 0.7F, AnimationEvidence::PredictedPronunciation}, 1.0F},
        {{Viseme::O, 0.6F, AnimationEvidence::PredictedPronunciation}, 2.0F},
    };
}

static void check_cue(const PredictedVisemeCue& cue,
                      std::uint64_t begin,
                      std::uint32_t count,
                      Viseme value) {
    CHECK(cue.sample_begin == begin);
    CHECK(cue.sample_count == count);
    CHECK(cue.estimate.value == value);
    CHECK(cue.estimate.evidence == AnimationEvidence::PredictedPronunciation);
}

int main() {
    try {
        const OnlinePredictiveVisemePolicy policy{120, 16};

        // Full output appears incrementally, while the incomplete current
        // interval remains unpublished until it is fully observed.
        OnlinePredictiveVisemePlanner planner(policy);
        planner.begin(7, 48000, 1000, sequence());
        CHECK(planner.state() == OnlinePredictiveVisemeState::Active);
        auto first = planner.observe_samples(1000, 480);
        CHECK(first.size() == 3);
        check_cue(first[0], 1000, 120, Viseme::Closed);
        check_cue(first[1], 1120, 240, Viseme::A);
        check_cue(first[2], 1360, 120, Viseme::FV);
        CHECK(planner.published_sample_end() == 1480);
        const auto pending = planner.observe_samples(1480, 40);
        CHECK(pending.empty());
        const auto final = planner.complete();
        CHECK(final.size() == 1);
        check_cue(final[0], 1480, 40, Viseme::O);
        CHECK(planner.state() == OnlinePredictiveVisemeState::Completed);
        CHECK(planner.complete().empty());

        bool late_rejected = false;
        try {
            (void)planner.observe_samples(1520, 1);
        } catch (const std::logic_error&) {
            late_rejected = true;
        }
        CHECK(late_rejected);

        // Final output is invariant under chunk partitioning.
        const auto cuts = std::vector<std::uint32_t>{120, 360, 480, 40};
        OnlinePredictiveVisemePlanner partitioned(policy);
        partitioned.begin(8, 48000, 1000, sequence());
        std::vector<PredictedVisemeCue> partitioned_cues;
        std::uint64_t position = 1000;
        for (const auto count : cuts) {
            auto batch = partitioned.observe_samples(position, count);
            partitioned_cues.insert(partitioned_cues.end(), batch.begin(), batch.end());
            position += count;
        }
        auto partitioned_tail = partitioned.complete();
        partitioned_cues.insert(partitioned_cues.end(),
                                 partitioned_tail.begin(), partitioned_tail.end());
        CHECK(partitioned_cues.size() == 4);
        CHECK(partitioned_cues[0].sample_begin == first[0].sample_begin);
        CHECK(partitioned_cues[1].sample_begin == first[1].sample_begin);
        CHECK(partitioned_cues[2].sample_begin == first[2].sample_begin);
        CHECK(partitioned_cues[3].sample_begin == final[0].sample_begin);

        // Pronunciation shorter than PCM leaves the rest uncovered for the
        // audio-reactive fallback; no invented prediction is emitted.
        OnlinePredictiveVisemePlanner short_pronunciation(policy);
        short_pronunciation.begin(9, 48000, 0, {
            {{Viseme::A, 1.0F, AnimationEvidence::PredictedPronunciation}, 1.0F},
        });
        const auto short_cues = short_pronunciation.observe_samples(0, 1000);
        CHECK(short_cues.size() == 1);
        CHECK(short_pronunciation.observe_samples(1000, 1000).empty());
        CHECK(short_pronunciation.complete().empty());

        // PCM shorter than pronunciation clips only the current observed
        // interval and drops units that were never reached.
        OnlinePredictiveVisemePlanner short_pcm(policy);
        short_pcm.begin(10, 48000, 5000, sequence());
        CHECK(short_pcm.observe_samples(5000, 130).size() == 1);
        const auto short_pcm_tail = short_pcm.complete();
        CHECK(short_pcm_tail.size() == 1);
        check_cue(short_pcm_tail[0], 5120, 10, Viseme::A);

        // Out-of-order, duplicate, and overlapping ranges are rejected
        // without changing the observed frontier.
        OnlinePredictiveVisemePlanner malformed(policy);
        malformed.begin(11, 48000, 100, sequence());
        CHECK(malformed.observe_samples(100, 120).size() == 1);
        const auto frontier = malformed.observed_sample_end();
        for (const auto bad_begin : {99ULL, 100ULL, 110ULL}) {
            bool rejected = false;
            try {
                (void)malformed.observe_samples(bad_begin, 10);
            } catch (const std::invalid_argument&) {
                rejected = true;
            }
            CHECK(rejected);
            CHECK(malformed.observed_sample_end() == frontier);
        }

        // Lifecycle: cancellation is idempotent, reset permits a new request,
        // and complete/cancel before PCM produce no prediction cues.
        OnlinePredictiveVisemePlanner lifecycle(policy);
        lifecycle.begin(12, 48000, 1000, sequence());
        CHECK(lifecycle.complete().empty());
        CHECK(lifecycle.complete().empty());
        lifecycle.reset();
        lifecycle.begin(13, 48000, 2000, sequence());
        lifecycle.cancel();
        lifecycle.cancel();
        CHECK(lifecycle.state() == OnlinePredictiveVisemeState::Cancelled);
        CHECK(lifecycle.published_sample_end() == 2000);
        lifecycle.reset();
        lifecycle.begin(14, 48000, 3000, sequence());
        CHECK(lifecycle.observe_samples(3000, 120).size() == 1);

        // Invalid timing policy and sequence evidence are rejected before
        // begin mutates the state.
        bool invalid_policy = false;
        try {
            OnlinePredictiveVisemePlanner bad_policy({0, 16});
            (void)bad_policy;
        } catch (const std::invalid_argument&) {
            invalid_policy = true;
        }
        CHECK(!invalid_policy); // policy is validated when an utterance begins
        OnlinePredictiveVisemePlanner no_hint({0, 16});
        bool no_hint_rejected = false;
        try {
            no_hint.begin(15, 48000, 0, sequence());
        } catch (const std::invalid_argument&) {
            no_hint_rejected = true;
        }
        CHECK(no_hint_rejected);
        CHECK(no_hint.state() == OnlinePredictiveVisemeState::Idle);

        auto invalid_sequence = sequence();
        invalid_sequence.front().estimate.evidence = AnimationEvidence::AudioReactive;
        bool invalid_evidence = false;
        try {
            planner.reset();
            planner.begin(16, 48000, 0, invalid_sequence);
        } catch (const std::invalid_argument&) {
            invalid_evidence = true;
        }
        CHECK(invalid_evidence);
        CHECK(planner.state() == OnlinePredictiveVisemeState::Idle);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
