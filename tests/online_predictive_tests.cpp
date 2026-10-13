#include <speech_animation/predictive.hpp>

#include <cmath>
#include <iostream>
#include <limits>
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

static void check_same_cues(const std::vector<PredictedVisemeCue>& left,
                            const std::vector<PredictedVisemeCue>& right) {
    CHECK(left.size() == right.size());
    for (std::size_t index = 0; index < left.size(); ++index) {
        CHECK(left[index].sample_begin == right[index].sample_begin);
        CHECK(left[index].sample_count == right[index].sample_count);
        CHECK(left[index].estimate.value == right[index].estimate.value);
        CHECK(left[index].estimate.confidence == right[index].estimate.confidence);
        CHECK(left[index].estimate.evidence == right[index].estimate.evidence);
    }
}

int main() {
    try {
        const OnlinePredictiveVisemePolicy policy{120, 16};

        // Full output appears incrementally, while the incomplete current
        // interval remains unpublished until it is fully observed.
        OnlinePredictiveVisemePlanner planner(policy);
        planner.begin(7, 48000, 1000, sequence());
        CHECK(planner.state() == OnlinePredictiveVisemeState::Active);
        auto first = planner.observe_samples(7, 1000, 480);
        CHECK(first.size() == 3);
        check_cue(first[0], 1000, 120, Viseme::Closed);
        check_cue(first[1], 1120, 240, Viseme::A);
        check_cue(first[2], 1360, 120, Viseme::FV);
        CHECK(planner.published_sample_end() == 1480);
        const auto pending = planner.observe_samples(7, 1480, 40);
        CHECK(pending.empty());
        const auto final = planner.complete(7);
        CHECK(final.size() == 1);
        check_cue(final[0], 1480, 40, Viseme::O);
        CHECK(planner.state() == OnlinePredictiveVisemeState::Completed);
        CHECK(planner.complete(7).empty());

        bool late_rejected = false;
        try {
            (void)planner.observe_samples(7, 1520, 1);
        } catch (const std::logic_error&) {
            late_rejected = true;
        }
        CHECK(late_rejected);

        // Final output is invariant under chunk partitioning.
        const auto cuts = std::vector<std::uint32_t>{80, 40, 160, 280, 120, 320};
        OnlinePredictiveVisemePlanner partitioned(policy);
        partitioned.begin(8, 48000, 1000, sequence());
        std::vector<PredictedVisemeCue> partitioned_cues;
        std::uint64_t position = 1000;
        for (const auto count : cuts) {
            auto batch = partitioned.observe_samples(8, position, count);
            partitioned_cues.insert(partitioned_cues.end(), batch.begin(), batch.end());
            position += count;
        }
        auto partitioned_tail = partitioned.complete(8);
        partitioned_cues.insert(partitioned_cues.end(),
                                 partitioned_tail.begin(), partitioned_tail.end());
        check_same_cues(partitioned_cues, {
            {1000, 120, {Viseme::Closed, 0.9F, AnimationEvidence::PredictedPronunciation}},
            {1120, 240, {Viseme::A, 0.8F, AnimationEvidence::PredictedPronunciation}},
            {1360, 120, {Viseme::FV, 0.7F, AnimationEvidence::PredictedPronunciation}},
            {1480, 240, {Viseme::O, 0.6F, AnimationEvidence::PredictedPronunciation}},
        });

        // The same PCM range must produce the same cues when delivered as a
        // single chunk, not only when compared with expected values.
        OnlinePredictiveVisemePlanner single_chunk(policy);
        single_chunk.begin(30, 48000, 1000, sequence());
        auto single_chunk_cues = single_chunk.observe_samples(30, 1000, 1000);
        auto single_chunk_tail = single_chunk.complete(30);
        single_chunk_cues.insert(single_chunk_cues.end(),
                                 single_chunk_tail.begin(),
                                 single_chunk_tail.end());
        check_same_cues(single_chunk_cues, partitioned_cues);

        // Pronunciation shorter than PCM leaves the rest uncovered for the
        // audio-reactive fallback; no invented prediction is emitted.
        OnlinePredictiveVisemePlanner short_pronunciation(policy);
        short_pronunciation.begin(9, 48000, 0, {
            {{Viseme::A, 1.0F, AnimationEvidence::PredictedPronunciation}, 1.0F},
        });
        const auto short_cues = short_pronunciation.observe_samples(9, 0, 1000);
        CHECK(short_cues.size() == 1);
        CHECK(short_pronunciation.observe_samples(9, 1000, 1000).empty());
        CHECK(short_pronunciation.complete(9).empty());

        // PCM shorter than pronunciation clips only the current observed
        // interval and drops units that were never reached.
        OnlinePredictiveVisemePlanner short_pcm(policy);
        short_pcm.begin(10, 48000, 5000, sequence());
        CHECK(short_pcm.observe_samples(10, 5000, 130).size() == 1);
        const auto short_pcm_tail = short_pcm.complete(10);
        CHECK(short_pcm_tail.size() == 1);
        check_cue(short_pcm_tail[0], 5120, 10, Viseme::A);

        // Out-of-order, duplicate, and overlapping ranges are rejected
        // without changing the observed frontier.
        OnlinePredictiveVisemePlanner malformed(policy);
        malformed.begin(11, 48000, 100, sequence());
        CHECK(malformed.observe_samples(11, 100, 120).size() == 1);
        const auto frontier = malformed.observed_sample_end();
        for (const auto bad_begin : {99ULL, 100ULL, 110ULL}) {
            bool rejected = false;
            try {
                (void)malformed.observe_samples(11, bad_begin, 10);
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
        CHECK(lifecycle.complete(12).empty());
        CHECK(lifecycle.complete(12).empty());
        lifecycle.reset();
        lifecycle.begin(13, 48000, 2000, sequence());
        lifecycle.cancel(13);
        lifecycle.cancel(13);
        CHECK(lifecycle.state() == OnlinePredictiveVisemeState::Cancelled);
        CHECK(lifecycle.published_sample_end() == 2000);
        bool cancelled_late_rejected = false;
        try {
            (void)lifecycle.observe_samples(13, 2000, 1);
        } catch (const std::logic_error&) {
            cancelled_late_rejected = true;
        }
        CHECK(cancelled_late_rejected);
        lifecycle.reset();
        lifecycle.begin(14, 48000, 3000, sequence());
        CHECK(lifecycle.observe_samples(14, 3000, 120).size() == 1);

        // An old generation cannot reuse a matching sample offset after reset
        // and begin of a replacement utterance.
        OnlinePredictiveVisemePlanner fenced(policy);
        fenced.begin(20, 48000, 0, sequence());
        fenced.cancel(20);
        fenced.reset();
        fenced.begin(21, 48000, 0, sequence());
        for (const auto action : {0, 1}) {
            bool stale_rejected = false;
            try {
                if (action == 0) {
                    (void)fenced.observe_samples(20, 0, 120);
                } else {
                    fenced.cancel(20);
                }
            } catch (const std::invalid_argument&) {
                stale_rejected = true;
            }
            CHECK(stale_rejected);
            CHECK(fenced.state() == OnlinePredictiveVisemeState::Active);
            CHECK(fenced.observed_sample_end() == 0);
        }
        bool stale_complete_rejected = false;
        try {
            (void)fenced.complete(20);
        } catch (const std::invalid_argument&) {
            stale_complete_rejected = true;
        }
        CHECK(stale_complete_rejected);
        CHECK(fenced.state() == OnlinePredictiveVisemeState::Active);
        CHECK(fenced.observe_samples(21, 0, 120).size() == 1);

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

        OnlinePredictiveVisemePlanner limited({120, 1});
        bool too_many_rejected = false;
        try {
            limited.begin(17, 48000, 0, sequence());
        } catch (const std::invalid_argument&) {
            too_many_rejected = true;
        }
        CHECK(too_many_rejected);
        CHECK(limited.state() == OnlinePredictiveVisemeState::Idle);

        OnlinePredictiveVisemePlanner overflowing(policy);
        bool begin_overflow_rejected = false;
        try {
            overflowing.begin(18, 48000,
                             std::numeric_limits<std::uint64_t>::max() - 10,
                             sequence());
        } catch (const std::invalid_argument&) {
            begin_overflow_rejected = true;
        }
        CHECK(begin_overflow_rejected);
        CHECK(overflowing.state() == OnlinePredictiveVisemeState::Idle);

        OnlinePredictiveVisemePlanner input_overflow({1, 16});
        input_overflow.begin(19, 48000,
                             std::numeric_limits<std::uint64_t>::max() - 1,
                             {{{Viseme::A, 1.0F,
                                AnimationEvidence::PredictedPronunciation}, 1.0F}});
        bool input_overflow_rejected = false;
        try {
            (void)input_overflow.observe_samples(
                19, std::numeric_limits<std::uint64_t>::max() - 1, 2);
        } catch (const std::invalid_argument&) {
            input_overflow_rejected = true;
        }
        CHECK(input_overflow_rejected);
        CHECK(input_overflow.observed_sample_end() ==
              std::numeric_limits<std::uint64_t>::max() - 1);

        for (const float weight : {std::numeric_limits<float>::quiet_NaN(),
                                   std::numeric_limits<float>::infinity(), -1.0F}) {
            auto invalid_weight = sequence();
            invalid_weight.front().duration_weight = weight;
            OnlinePredictiveVisemePlanner bad_weight(policy);
            bool weight_rejected = false;
            try {
                bad_weight.begin(22, 48000, 0, invalid_weight);
            } catch (const std::invalid_argument&) {
                weight_rejected = true;
            }
            CHECK(weight_rejected);
            CHECK(bad_weight.state() == OnlinePredictiveVisemeState::Idle);
        }
        for (const float confidence : {std::numeric_limits<float>::quiet_NaN(), -0.1F, 1.1F}) {
            auto invalid_confidence = sequence();
            invalid_confidence.front().estimate.confidence = confidence;
            OnlinePredictiveVisemePlanner bad_confidence(policy);
            bool confidence_rejected = false;
            try {
                bad_confidence.begin(23, 48000, 0, invalid_confidence);
            } catch (const std::invalid_argument&) {
                confidence_rejected = true;
            }
            CHECK(confidence_rejected);
            CHECK(bad_confidence.state() == OnlinePredictiveVisemeState::Idle);
        }

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
