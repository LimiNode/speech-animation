#include <speech_animation/speech_animation.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace speech_animation;

static void check(bool condition, const char* expression, const char* file, int line) {
    if (!condition) {
        throw std::runtime_error(std::string(file) + ":" + std::to_string(line) +
                                 ": CHECK failed: " + expression);
    }
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

static std::vector<SpeechAnimationSpan> run_partition(const std::vector<float>& pcm,
                                                       const std::vector<std::size_t>& cuts) {
    SpeechAnimationAnalyzer analyzer;
    std::vector<SpeechAnimationSpan> result;
    std::size_t begin = 0;
    for (const std::size_t count : cuts) {
        PcmChunk chunk{7, begin, 48000, pcm.data() + begin,
                       static_cast<std::uint32_t>(count)};
        auto spans = analyzer.feed(chunk);
        result.insert(result.end(), spans.begin(), spans.end());
        begin += count;
    }
    CHECK(begin == pcm.size());
    return result;
}

int main() {
    try {
        std::vector<float> pcm(5000);
        for (std::size_t i = 0; i < pcm.size(); ++i) {
            pcm[i] = (i % 97 < 48) ? 0.7F : 0.0F;
        }

        const auto one = run_partition(pcm, {pcm.size()});
        const auto irregular = run_partition(pcm, {13, 1, 777, 409, 3800});
        const auto one_sample = run_partition(pcm, std::vector<std::size_t>(pcm.size(), 1));
        CHECK(one.size() == pcm.size());
        CHECK(irregular.size() == one.size());
        CHECK(one_sample.size() == one.size());
        for (std::size_t i = 0; i < one.size(); ++i) {
            CHECK(one[i].sample_begin == irregular[i].sample_begin);
            CHECK(one[i].energy == irregular[i].energy);
            CHECK(one[i].mouth_open == irregular[i].mouth_open);
            CHECK(one[i].activity == irregular[i].activity);
            CHECK(one[i].energy == one_sample[i].energy);
            CHECK(one[i].mouth_open == one_sample[i].mouth_open);
        }

        SpeechAnimationAnalyzer analyzer;
        PcmChunk first{9, 100, 48000, pcm.data(), 10};
        (void)analyzer.feed(first);
        bool rejected = false;
        try {
            PcmChunk out_of_order{9, 50, 48000, pcm.data(), 1};
            (void)analyzer.feed(out_of_order);
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        CHECK(rejected);
        PcmChunk in_order{9, 110, 48000, pcm.data(), 1};
        CHECK(analyzer.feed(in_order).front().sample_begin == 110);
        CHECK(analyzer.flush().empty());

        // Reset must be equivalent to constructing a fresh analyzer.
        SpeechAnimationAnalyzer reset_analyzer;
        PcmChunk utterance_a{10, 0, 48000, pcm.data(), 200};
        (void)reset_analyzer.feed(utterance_a);
        reset_analyzer.reset();
        PcmChunk utterance_b{11, 0, 48000, pcm.data() + 200, 300};
        const auto after_reset = reset_analyzer.feed(utterance_b);

        SpeechAnimationAnalyzer fresh_analyzer;
        const auto fresh = fresh_analyzer.feed(utterance_b);
        CHECK(after_reset.size() == fresh.size());
        for (std::size_t i = 0; i < fresh.size(); ++i) {
            CHECK(after_reset[i].utterance_id == fresh[i].utterance_id);
            CHECK(after_reset[i].sample_begin == fresh[i].sample_begin);
            CHECK(after_reset[i].sample_count == fresh[i].sample_count);
            CHECK(after_reset[i].sample_rate == fresh[i].sample_rate);
            CHECK(after_reset[i].energy == fresh[i].energy);
            CHECK(after_reset[i].mouth_open == fresh[i].mouth_open);
            CHECK(after_reset[i].activity == fresh[i].activity);
            CHECK(after_reset[i].viseme.has_value() == fresh[i].viseme.has_value());
        }

        AnalyzerConfig invalid;
        invalid.energy_attack_ms = std::numeric_limits<float>::quiet_NaN();
        bool invalid_rejected = false;
        try {
            SpeechAnimationAnalyzer bad(invalid);
        } catch (const std::invalid_argument&) {
            invalid_rejected = true;
        }
        CHECK(invalid_rejected);

        float malformed_samples[] = {
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::infinity(),
            2.0F,
        };
        SpeechAnimationAnalyzer sanitizing_analyzer;
        const auto sanitized = sanitizing_analyzer.feed(
            PcmChunk{12, 0, 48000, malformed_samples, 3});
        CHECK(sanitized.size() == 3);
        for (const auto& span : sanitized) {
            CHECK(std::isfinite(span.energy));
            CHECK(std::isfinite(span.mouth_open));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
