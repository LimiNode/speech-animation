#include <speech_animation/experimental/spectral.hpp>
#include <speech_animation/speech_animation.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <utility>
#include <vector>

namespace {

struct Result {
    double elapsed_ms = 0.0;
    std::size_t output_cues = 0;
    std::size_t confident_cues = 0;
    std::size_t viseme_switches = 0;
    double mean_weight_delta = 0.0;
};

std::vector<float> make_fixture(std::uint32_t sample_rate, std::size_t seconds) {
    std::vector<float> pcm(sample_rate * seconds);
    constexpr float frequencies[] = {440.0F, 800.0F, 2500.0F, 600.0F, 1200.0F};
    for (std::size_t index = 0; index < pcm.size(); ++index) {
        const auto segment = (index / (sample_rate / 5U)) % 5U;
        const double time = static_cast<double>(index) / sample_rate;
        const double envelope = 0.35 + 0.20 *
            std::sin(2.0 * 3.141592653589793 * 2.0 * time);
        pcm[index] = static_cast<float>(envelope * std::sin(
            2.0 * 3.141592653589793 * frequencies[segment] * time));
    }
    return pcm;
}

std::vector<float> make_amplitude_fixture(std::uint32_t sample_rate,
                                          std::size_t seconds) {
    std::vector<float> pcm(sample_rate * seconds);
    for (std::size_t index = 0; index < pcm.size(); ++index) {
        const double time = static_cast<double>(index) / sample_rate;
        float envelope = 0.0F;
        if (time >= 0.5 && time < 1.0) {
            envelope = 0.06F;
        } else if (time >= 1.0 && time < 2.0) {
            envelope = static_cast<float>(0.06 + 0.54 * (time - 1.0));
        } else if (time >= 2.0 && time < 3.0) {
            envelope = 0.60F;
        } else if (time >= 3.5) {
            envelope = static_cast<float>(0.25 +
                0.25 * std::sin(2.0 * 3.141592653589793 * 2.0 * (time - 3.5)));
        }
        pcm[index] = envelope * static_cast<float>(std::sin(
            2.0 * 3.141592653589793 * 440.0 * time));
    }
    return pcm;
}

Result run_baseline(std::size_t chunk_samples,
                    const std::vector<float>& pcm,
                    std::uint32_t sample_rate) {
    speech_animation::AnalyzerConfig config;
    config.output_hop_samples = sample_rate / 100U;
    speech_animation::SpeechAnimationAnalyzer analyzer(config);
    std::size_t output = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t offset = 0; offset < pcm.size(); offset += chunk_samples) {
        const auto count = std::min(chunk_samples, pcm.size() - offset);
        output += analyzer.feed({1, static_cast<std::uint64_t>(offset), sample_rate,
                                 pcm.data() + offset, static_cast<std::uint32_t>(count)}).size();
    }
    output += analyzer.flush().size();
    const auto end = std::chrono::steady_clock::now();
    return {std::chrono::duration<double, std::milli>(end - start).count(), output,
            0, 0, 0.0};
}

Result run_experimental(std::size_t chunk_samples,
                        const std::vector<float>& pcm,
                        std::uint32_t sample_rate,
                        speech_animation::experimental::AudioReactiveVisemeMode mode) {
    speech_animation::experimental::AudioReactiveVisemeConfig config;
    config.mode = mode;
    config.window_samples = sample_rate * 30U / 1000U;
    config.output_hop_samples = sample_rate / 100U;
    speech_animation::experimental::AudioReactiveVisemeAnalyzer analyzer(config);
    std::size_t output = 0;
    std::size_t confident = 0;
    std::size_t switches = 0;
    std::size_t weight_pairs = 0;
    double weight_delta = 0.0;
    bool have_previous = false;
    speech_animation::experimental::AudioReactiveVisemeWeights previous;
    speech_animation::Viseme previous_value = speech_animation::Viseme::Neutral;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t offset = 0; offset < pcm.size(); offset += chunk_samples) {
        const auto count = std::min(chunk_samples, pcm.size() - offset);
        const auto cues = analyzer.feed({1, static_cast<std::uint64_t>(offset), sample_rate,
                                         pcm.data() + offset, static_cast<std::uint32_t>(count)});
        output += cues.size();
        for (const auto& cue : cues) {
            if (cue.viseme.has_value()) {
                ++confident;
                if (have_previous && cue.viseme->value != previous_value) {
                    ++switches;
                }
                previous_value = cue.viseme->value;
            }
            if (have_previous) {
                weight_delta += std::abs(cue.weights.a - previous.a) +
                                std::abs(cue.weights.e - previous.e) +
                                std::abs(cue.weights.i - previous.i) +
                                std::abs(cue.weights.o - previous.o) +
                                std::abs(cue.weights.u - previous.u);
                ++weight_pairs;
            }
            previous = cue.weights;
            have_previous = true;
        }
    }
    const auto tail = analyzer.flush();
    output += tail.size();
    for (const auto& cue : tail) {
        if (cue.viseme.has_value()) {
            ++confident;
            if (have_previous && cue.viseme->value != previous_value) {
                ++switches;
            }
            previous_value = cue.viseme->value;
        }
        if (have_previous) {
            weight_delta += std::abs(cue.weights.a - previous.a) +
                            std::abs(cue.weights.e - previous.e) +
                            std::abs(cue.weights.i - previous.i) +
                            std::abs(cue.weights.o - previous.o) +
                            std::abs(cue.weights.u - previous.u);
            ++weight_pairs;
        }
        previous = cue.weights;
        have_previous = true;
    }
    const auto end = std::chrono::steady_clock::now();
    return {std::chrono::duration<double, std::milli>(end - start).count(), output,
            confident, switches,
            weight_pairs == 0 ? 0.0 : weight_delta / static_cast<double>(weight_pairs)};
}

} // namespace

int main() {
    constexpr std::uint32_t sample_rate = 48000;
    constexpr std::size_t seconds = 5;
    const auto voiced_pcm = make_fixture(sample_rate, seconds);
    const auto amplitude_pcm = make_amplitude_fixture(sample_rate, seconds);
    std::cout << "fixture,variant,chunk_samples,elapsed_ms,realtime_factor,output_cues,"
                 "confident_cues,viseme_switches,mean_weight_delta\n";
    for (const auto& fixture : {std::pair{"voiced", &voiced_pcm},
                                std::pair{"amplitude", &amplitude_pcm}}) {
        for (const auto chunk_samples : {std::size_t{80}, std::size_t{480},
                                         std::size_t{1920}}) {
            const auto baseline = run_baseline(chunk_samples, *fixture.second, sample_rate);
            const auto amplitude = run_experimental(
                chunk_samples, *fixture.second, sample_rate,
                speech_animation::experimental::AudioReactiveVisemeMode::Amplitude);
            const auto spectral = run_experimental(
                chunk_samples, *fixture.second, sample_rate,
                speech_animation::experimental::AudioReactiveVisemeMode::Spectral);
            for (const auto& named : {std::pair{"baseline", baseline},
                                      std::pair{"amplitude", amplitude},
                                      std::pair{"spectral", spectral}}) {
                const double factor = (1000.0 * static_cast<double>(seconds)) /
                                      std::max(0.001, named.second.elapsed_ms);
                std::cout << fixture.first << ',' << named.first << ',' << chunk_samples
                          << ',' << std::fixed << std::setprecision(3)
                          << named.second.elapsed_ms << ',' << factor << ','
                          << named.second.output_cues << ','
                          << named.second.confident_cues << ','
                          << named.second.viseme_switches << ','
                          << named.second.mean_weight_delta << '\n';
            }
        }
    }
}
