#include <speech_animation/predictive.hpp>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace speech_animation;

static void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error("predictive test line " + std::to_string(line) +
                                 ": CHECK failed: " + expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

int main() {
    try {
        const std::vector<PronunciationUnit> pronunciation = {
            {"p", 1.0F, 1.0F},
            {"a", 0.9F, 2.0F},
            {"f", 0.8F, 1.0F},
            {"unknown", 1.0F, 1.0F},
        };
        const auto specs = PronunciationVisemeMapper::map_sequence(pronunciation);
        CHECK(specs.size() == 4);
        CHECK(specs[0].value == Viseme::Closed);
        CHECK(specs[1].value == Viseme::A);
        CHECK(specs[2].value == Viseme::FV);
        CHECK(specs[3].value == Viseme::Neutral);
        CHECK(specs[3].confidence < 1.0F);

        const SpeechContext context{77, 1000, 48000, 1000};
        PredictiveVisemePlanner planner;
        const auto cues = planner.plan(context, specs);
        CHECK(cues.size() == specs.size());
        CHECK(cues.front().sample_begin == 1000);
        CHECK(cues.front().sample_count == 200);
        CHECK(cues[1].sample_count == 400);
        CHECK(cues[2].sample_count == 200);
        CHECK(cues.back().sample_count == 200);
        std::uint64_t next = context.sample_begin;
        for (const auto& cue : cues) {
            CHECK(cue.utterance_id == context.utterance_id);
            CHECK(cue.sample_rate == context.sample_rate);
            CHECK(cue.sample_begin == next);
            CHECK(cue.sample_count != 0);
            CHECK(cue.viseme.evidence == AnimationEvidence::PredictedPronunciation);
            CHECK(std::isfinite(cue.viseme.confidence));
            next += cue.sample_count;
        }
        CHECK(next == context.sample_begin + context.total_sample_count);

        PredictiveVisemePlanner bound_planner(context);
        const auto bound_cues = bound_planner.plan(specs);
        CHECK(bound_cues.size() == cues.size());
        for (std::size_t i = 0; i < cues.size(); ++i) {
            CHECK(bound_cues[i].sample_begin == cues[i].sample_begin);
            CHECK(bound_cues[i].sample_count == cues[i].sample_count);
        }

        bool invalid_context = false;
        try {
            (void)planner.plan(SpeechContext{0, 0, 48000, 100}, specs);
        } catch (const std::invalid_argument&) {
            invalid_context = true;
        }
        CHECK(invalid_context);

        bool no_bound_context = false;
        try {
            (void)planner.plan(specs);
        } catch (const std::logic_error&) {
            no_bound_context = true;
        }
        CHECK(no_bound_context);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
