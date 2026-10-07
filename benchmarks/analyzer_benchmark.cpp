#include <speech_animation/speech_animation.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {

struct RunResult {
    double elapsed_ms = 0.0;
    std::size_t output_spans = 0;
};

RunResult run(std::size_t chunk_samples, const std::vector<float>& pcm) {
    using clock = std::chrono::steady_clock;
    speech_animation::AnalyzerConfig config;
    config.output_hop_samples = 480; // 10 ms at 48 kHz.
    speech_animation::SpeechAnimationAnalyzer analyzer(config);

    std::size_t output_spans = 0;
    const auto begin = clock::now();
    for (std::size_t offset = 0; offset < pcm.size(); offset += chunk_samples) {
        const auto count = std::min(chunk_samples, pcm.size() - offset);
        const speech_animation::PcmChunk chunk{
            1,
            static_cast<std::uint64_t>(offset),
            48000,
            pcm.data() + offset,
            static_cast<std::uint32_t>(count),
        };
        output_spans += analyzer.feed(chunk).size();
    }
    output_spans += analyzer.flush().size();
    const auto end = clock::now();
    return {
        std::chrono::duration<double, std::milli>(end - begin).count(),
        output_spans,
    };
}

} // namespace

int main() {
    constexpr std::uint32_t sample_rate = 48000;
    constexpr std::size_t seconds = 5;
    std::vector<float> pcm(sample_rate * seconds);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        const double modulation = 0.35 + 0.25 * std::sin(2.0 * 3.141592653589793 * 3.0 * t);
        pcm[i] = static_cast<float>(modulation *
                                    std::sin(2.0 * 3.141592653589793 * 220.0 * t));
    }

    std::cout << "audio_seconds=" << seconds << " sample_rate=" << sample_rate << '\n';
    std::cout << "chunk_samples,elapsed_ms,realtime_factor,output_spans\n";
    for (const std::size_t chunk_samples : {std::size_t{1}, std::size_t{80},
                                             std::size_t{480}, std::size_t{1920},
                                             std::size_t{3840}}) {
        const auto result = run(chunk_samples, pcm);
        const double realtime_factor = (1000.0 * static_cast<double>(seconds)) /
                                       std::max(0.001, result.elapsed_ms);
        std::cout << chunk_samples << ',' << std::fixed << std::setprecision(3)
                  << result.elapsed_ms << ',' << realtime_factor << ','
                  << result.output_spans << '\n';
    }
    return 0;
}
