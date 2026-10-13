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
        const auto mapped = PronunciationVisemeMapper::map_sequence({
            {"p", 1.0F, 1.0F}, {"a", 0.9F, 2.0F}, {"f", 0.8F, 1.0F},
            {"unsupported", 1.0F, 1.0F},
        });
        CHECK(mapped.size() == 4);
        CHECK(mapped[0].estimate.value == Viseme::Closed);
        CHECK(mapped[1].estimate.value == Viseme::A);
        CHECK(mapped[2].estimate.value == Viseme::FV);
        CHECK(mapped[3].estimate.value == Viseme::Neutral);
        CHECK(mapped[3].estimate.confidence < 1.0F);
        CHECK(mapped[1].estimate.evidence == AnimationEvidence::PredictedPronunciation);
        CHECK(PronunciationVisemeMapper::map(PronunciationUnit{"\xD0\xB0", 1.0F, 1.0F}).estimate.value == Viseme::A);
        CHECK(PronunciationVisemeMapper::map(PronunciationUnit{"\xD0\xBE", 1.0F, 1.0F}).estimate.value == Viseme::O);

        PredictiveVisemePlanner mapped_planner(SpeechContext{41, 48000, 800, "mafa"});
        const auto mapped_cues = mapped_planner.plan(mapped);
        CHECK(mapped_cues.size() == mapped.size());
        CHECK(mapped_cues.front().sample_begin == 0);
        CHECK(mapped_cues.back().sample_begin + mapped_cues.back().sample_count == 800);

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

        SpeechContext offset_context{43, 48000, 1000, "ma-ka", 2000};
        PredictiveVisemePlanner offset_planner(offset_context);
        const auto offset_cues = offset_planner.plan({
            {{Viseme::A, 1.0F, AnimationEvidence::PredictedPronunciation}, 1.0F},
        });
        CHECK(offset_cues.front().sample_begin == 2000);

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
