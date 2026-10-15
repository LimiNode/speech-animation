#include <speech_animation/experimental/spectral.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace speech_animation;
using namespace speech_animation::experimental;

static void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error("CHECK failed at line " + std::to_string(line) +
                                 ": " + expression);
    }
}

#define CHECK(expression) check((expression), #expression, __LINE__)

static std::vector<float> sine(std::uint32_t sample_rate,
                               float frequency,
                               std::size_t count) {
    std::vector<float> result(count);
    for (std::size_t index = 0; index < count; ++index) {
        result[index] = 0.6F * std::sin(
            2.0F * 3.14159265358979323846F * frequency *
            static_cast<float>(index) / static_cast<float>(sample_rate));
    }
    return result;
}

static std::vector<AudioReactiveVisemeCue> run_partition(
    const std::vector<float>& pcm,
    std::uint32_t sample_rate,
    const std::vector<std::size_t>& cuts,
    AudioReactiveVisemeConfig config = {}) {
    AudioReactiveVisemeAnalyzer analyzer(config);
    std::vector<AudioReactiveVisemeCue> result;
    std::size_t offset = 0;
    for (const auto count : cuts) {
        const PcmChunk chunk{17, static_cast<std::uint64_t>(offset), sample_rate,
                             pcm.data() + offset, static_cast<std::uint32_t>(count)};
        auto batch = analyzer.feed(chunk);
        result.insert(result.end(), batch.begin(), batch.end());
        offset += count;
    }
    auto tail = analyzer.flush();
    result.insert(result.end(), tail.begin(), tail.end());
    CHECK(offset == pcm.size());
    return result;
}

static void check_same_cues(const std::vector<AudioReactiveVisemeCue>& left,
                            const std::vector<AudioReactiveVisemeCue>& right) {
    CHECK(left.size() == right.size());
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto& a = left[index];
        const auto& b = right[index];
        CHECK(a.utterance_id == b.utterance_id);
        CHECK(a.sample_begin == b.sample_begin);
        CHECK(a.sample_count == b.sample_count);
        CHECK(a.sample_rate == b.sample_rate);
        CHECK(a.energy == b.energy);
        CHECK(a.mouth_open == b.mouth_open);
        CHECK(a.activity == b.activity);
        CHECK(a.weights.a == b.weights.a);
        CHECK(a.weights.e == b.weights.e);
        CHECK(a.weights.i == b.weights.i);
        CHECK(a.weights.o == b.weights.o);
        CHECK(a.weights.u == b.weights.u);
        CHECK(a.viseme.has_value() == b.viseme.has_value());
        if (a.viseme.has_value()) {
            CHECK(a.viseme->value == b.viseme->value);
            CHECK(a.viseme->confidence == b.viseme->confidence);
            CHECK(a.viseme->evidence == b.viseme->evidence);
        }
    }
}

int main() {
    try {
        constexpr std::uint32_t sample_rate = 48000;
        const auto pcm = sine(sample_rate, 440.0F, 2400);

        // The result is independent of input chunk partitioning and remains
        // on the absolute PCM sample clock.
        const auto one = run_partition(pcm, sample_rate, {pcm.size()});
        const auto irregular = run_partition(pcm, sample_rate, {1, 79, 333, 887, 1100});
        check_same_cues(one, irregular);
        std::uint64_t covered = 0;
        for (const auto& cue : one) {
            CHECK(cue.sample_begin == covered);
            CHECK(cue.sample_count != 0);
            CHECK(cue.sample_rate == sample_rate);
            CHECK(cue.utterance_id == 17);
            CHECK(cue.viseme.has_value() || cue.activity == SpeechActivity::Silence ||
                  cue.energy > 0.0F);
            covered += cue.sample_count;
        }
        CHECK(covered == pcm.size());

        // No cue is available before the first output hop is observed.
        AudioReactiveVisemeAnalyzer causal;
        const auto short_pcm = sine(sample_rate, 440.0F, 479);
        CHECK(causal.feed(PcmChunk{18, 100, sample_rate, short_pcm.data(),
                                   static_cast<std::uint32_t>(short_pcm.size())}).empty());
        const auto first = sine(sample_rate, 440.0F, 1);
        const auto available = causal.feed(PcmChunk{18, 579, sample_rate,
                                                    first.data(), 1});
        CHECK(available.size() == 1);
        CHECK(available.front().sample_begin == 100);
        CHECK(available.front().sample_count == 480);

        // Window and output hop are independent, but publication never runs
        // past the observed frontier. Warm-up uses a shortened causal frame.
        for (const auto window : {80U, 480U, 960U}) {
            AudioReactiveVisemeConfig boundary;
            boundary.window_samples = window;
            boundary.output_hop_samples = 480;
            AudioReactiveVisemeAnalyzer analyzer(boundary);
            const auto samples = sine(sample_rate, 440.0F, 960);
            CHECK(analyzer.feed(PcmChunk{19, 0, sample_rate, samples.data(), 479}).empty());
            const auto first_hop = analyzer.feed(
                PcmChunk{19, 479, sample_rate, samples.data() + 479, 1});
            CHECK(first_hop.size() == 1);
            CHECK(first_hop.front().sample_begin == 0);
            CHECK(first_hop.front().sample_count == 480);
            CHECK(first_hop.front().sample_begin + first_hop.front().sample_count <= 480);
        }

        // A short utterance produces a clipped, observed tail rather than a
        // fabricated full hop.
        AudioReactiveVisemeConfig short_config;
        short_config.window_samples = 960;
        short_config.output_hop_samples = 480;
        AudioReactiveVisemeAnalyzer short_analyzer(short_config);
        const auto short_utterance = sine(sample_rate, 440.0F, 100);
        CHECK(short_analyzer.feed(PcmChunk{20, 0, sample_rate,
                                           short_utterance.data(), 100}).empty());
        const auto short_tail = short_analyzer.flush();
        CHECK(short_tail.size() == 1);
        CHECK(short_tail.front().sample_begin == 0);
        CHECK(short_tail.front().sample_count == 100);

        // Silence has no confident shape and never fabricates pronunciation.
        const std::vector<float> silence(2000, 0.0F);
        const auto silent = run_partition(silence, sample_rate, {silence.size()});
        for (const auto& cue : silent) {
            CHECK(cue.activity == SpeechActivity::Silence);
            CHECK(!cue.viseme.has_value());
            CHECK(cue.weights.a == 0.0F && cue.weights.e == 0.0F &&
                  cue.weights.i == 0.0F && cue.weights.o == 0.0F && cue.weights.u == 0.0F);
        }
        AudioReactiveVisemeConfig amplitude_silence;
        amplitude_silence.mode = AudioReactiveVisemeMode::Amplitude;
        const auto silent_amplitude = run_partition(
            silence, sample_rate, {silence.size()}, amplitude_silence);
        for (const auto& cue : silent_amplitude) {
            CHECK(cue.activity == SpeechActivity::Silence);
            CHECK(!cue.viseme.has_value());
            CHECK(cue.weights.a == 0.0F && cue.weights.e == 0.0F &&
                  cue.weights.i == 0.0F && cue.weights.o == 0.0F && cue.weights.u == 0.0F);
        }

        // Frequencies are specified in Hz, so the same fixture remains valid
        // at all supported sample rates.
        for (const auto rate : {16000U, 24000U, 48000U}) {
            const auto rate_pcm = sine(rate, 440.0F, rate / 10U);
            const auto cues = run_partition(rate_pcm, rate, {rate_pcm.size()});
            CHECK(!cues.empty());
            std::uint32_t total = 0;
            for (const auto& cue : cues) {
                total += cue.sample_count;
            }
            CHECK(total == rate_pcm.size());
        }

        // The spectral hypothesis changes with band energy; it is not a
        // phoneme claim and is explicitly bounded to AudioReactive evidence.
        const auto low = run_partition(sine(sample_rate, 440.0F, 2400),
                                       sample_rate, {2400});
        const auto high = run_partition(sine(sample_rate, 3000.0F, 2400),
                                        sample_rate, {2400});
        CHECK(!low.empty() && !high.empty());
        CHECK(low.front().weights.a != high.front().weights.a ||
              low.front().weights.e != high.front().weights.e ||
              low.front().weights.i != high.front().weights.i ||
              low.front().weights.o != high.front().weights.o ||
              low.front().weights.u != high.front().weights.u);
        for (const auto& cue : low) {
            if (cue.viseme.has_value()) {
                CHECK(cue.viseme->evidence == AnimationEvidence::AudioReactive);
                CHECK(cue.viseme->confidence <= 0.60F);
            }
        }

        // Variant B is amplitude-driven and remains deterministic without a
        // spectral dependency.
        AudioReactiveVisemeConfig amplitude;
        amplitude.mode = AudioReactiveVisemeMode::Amplitude;
        const auto amplitude_cues = run_partition(pcm, sample_rate, {pcm.size()}, amplitude);
        CHECK(!amplitude_cues.empty());
        bool saw_amplitude_shape = false;
        for (const auto& cue : amplitude_cues) {
            saw_amplitude_shape = saw_amplitude_shape ||
                cue.weights.a + cue.weights.e + cue.weights.i +
                cue.weights.o + cue.weights.u > 0.0F;
            if (cue.viseme.has_value()) {
                CHECK(cue.viseme->evidence == AnimationEvidence::AudioReactive);
                CHECK(cue.viseme->confidence <= amplitude.maximum_confidence);
            }
        }
        CHECK(saw_amplitude_shape);

        AudioReactiveVisemeConfig invalid;
        invalid.window_ms = std::numeric_limits<float>::quiet_NaN();
        bool rejected = false;
        try {
            AudioReactiveVisemeAnalyzer bad(invalid);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        CHECK(rejected);

        AudioReactiveVisemeConfig oversized;
        oversized.window_samples = 65537;
        AudioReactiveVisemeAnalyzer bounded(oversized);
        const auto one_sample = sine(sample_rate, 440.0F, 1);
        bool oversized_rejected = false;
        try {
            (void)bounded.feed(PcmChunk{21, 0, sample_rate, one_sample.data(), 1});
        } catch (const std::invalid_argument&) {
            oversized_rejected = true;
        }
        CHECK(oversized_rejected);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
