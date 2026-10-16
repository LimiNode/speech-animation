#include "speech_animation/live.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

using namespace speech_animation::integration;

int main() {
    LiveAudioSessionConfig config;
    config.queue_capacity = 32;
    config.analyzer.output_hop_samples = 480;
    LiveAudioReactiveSession session(config);
    constexpr std::uint32_t sample_rate = 48000;
    constexpr std::uint32_t total_samples = sample_rate * 5;
    constexpr std::uint32_t chunk_size = 480;
    std::vector<float> samples(chunk_size);
    const auto start = std::chrono::steady_clock::now();
    std::size_t cue_count = 0;
    for (std::uint32_t first = 0; first < total_samples; first += chunk_size) {
        for (std::uint32_t index = 0; index < chunk_size; ++index) {
            samples[index] = 0.4F * std::sin(
                2.0 * 3.141592653589793 * 220.0 * (first + index) / sample_rate);
        }
        LivePcmChunk chunk;
        chunk.source_id = 1;
        chunk.source_epoch = 1;
        chunk.first_sample = first;
        chunk.sample_rate = sample_rate;
        chunk.pcm = samples;
        while (session.push(std::move(chunk)) == QueuePushResult::Full) {
            const auto cues = session.process_available();
            cue_count += cues.size();
            chunk.source_id = 1;
            chunk.first_sample = first;
            chunk.sample_rate = sample_rate;
            chunk.pcm = samples;
        }
        const auto cues = session.process_available();
        cue_count += cues.size();
    }
    session.stop();
    cue_count += session.process_available().size();
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "seconds=5 elapsed=" << elapsed
              << " realtime_factor=" << (5.0 / elapsed)
              << " cues=" << cue_count << '\n';
    return 0;
}
