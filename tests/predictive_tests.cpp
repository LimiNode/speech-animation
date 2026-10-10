#include <speech_animation/speech_animation.hpp>

#include <iostream>
#include <stdexcept>
#include <string>

using namespace speech_animation;

static void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error("CHECK failed at line " + std::to_string(line) +
                                 ": " + expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

int main() {
    try {
        SpeechContext context{42, 48000, 1000, "ma-ka"};
        PredictiveVisemePlanner planner(context);
        const auto cues = planner.plan({
            {{Viseme::A, 0.9F, AnimationEvidence::PredictedPronunciation}, 1.0F},
            {{Viseme::Closed, 0.8F, AnimationEvidence::PredictedPronunciation}, 1.0F},
            {{Viseme::A, 0.7F, AnimationEvidence::PredictedPronunciation}, 2.0F},
        });
        CHECK(cues.size() == 3);
        CHECK(cues[0].sample_begin == 0 && cues[0].sample_count == 250);
        CHECK(cues[1].sample_begin == 250 && cues[1].sample_count == 250);
        CHECK(cues[2].sample_begin == 500 && cues[2].sample_count == 500);
        CHECK(cues[2].sample_begin + cues[2].sample_count == context.total_sample_count);

        SpeechAnimationSpan span{42, 600, 100, 48000, 0.0F, 0.0F, std::nullopt,
                                 SpeechActivity::Active};
        const auto annotated = planner.annotate(span, cues);
        CHECK(annotated.viseme.has_value());
        CHECK(annotated.viseme->value == Viseme::A);
        CHECK(annotated.viseme->evidence == AnimationEvidence::PredictedPronunciation);

        SpeechAnimationSpan outside = span;
        outside.sample_begin = 1000;
        outside.viseme = VisemeEstimate{
            Viseme::Closed, 1.0F, AnimationEvidence::AudioReactive};
        CHECK(!planner.annotate(outside, cues).viseme.has_value());

        bool rejected_audio_evidence = false;
        try {
            (void)planner.plan({{{Viseme::A, 1.0F, AnimationEvidence::AudioReactive}, 1.0F}});
        } catch (const std::invalid_argument&) {
            rejected_audio_evidence = true;
        }
        CHECK(rejected_audio_evidence);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
