#include <speech_animation/speech_animation.hpp>

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace speech_animation;

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
    assert(begin == pcm.size());
    return result;
}

int main() {
    std::vector<float> pcm(5000);
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        pcm[i] = (i % 97 < 48) ? 0.7F : 0.0F;
    }

    const auto one = run_partition(pcm, {pcm.size()});
    const auto irregular = run_partition(pcm, {13, 1, 777, 409, 3800});
    const auto one_sample = [&]() {
        return run_partition(pcm, std::vector<std::size_t>(pcm.size(), 1));
    }();
    assert(one.size() == pcm.size());
    assert(irregular.size() == one.size());
    assert(one_sample.size() == one.size());
    for (std::size_t i = 0; i < one.size(); ++i) {
        assert(one[i].sample_begin == irregular[i].sample_begin);
        assert(one[i].energy == irregular[i].energy);
        assert(one[i].mouth_open == irregular[i].mouth_open);
        assert(one[i].activity == irregular[i].activity);
        assert(one[i].energy == one_sample[i].energy);
        assert(one[i].mouth_open == one_sample[i].mouth_open);
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
    assert(rejected);
    PcmChunk in_order{9, 110, 48000, pcm.data(), 1};
    assert(analyzer.feed(in_order).front().sample_begin == 110);
    assert(analyzer.flush().empty());

    analyzer.reset();
    PcmChunk after_reset{11, 0, 48000, pcm.data(), 1};
    auto reset_span = analyzer.feed(after_reset);
    assert(reset_span.front().utterance_id == 11);
    assert(reset_span.front().sample_begin == 0);
    assert(!reset_span.front().viseme.has_value());
    return 0;
}
