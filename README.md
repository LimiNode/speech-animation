# speech-animation

Engine-agnostic, sample-addressed realtime speech animation cues.

The v0 analyzer is a deterministic causal PCM baseline. It exposes normalized energy,
mouth opening, speech activity, and an optional lightweight voiced hint. It does
not import or know about Qwen, PocketTTS, xVibe, Unity, Godot, VRM, or Live2D.

## v0 usage

```cpp
#include <speech_animation/speech_animation.hpp>

speech_animation::SpeechAnimationAnalyzer analyzer;
speech_animation::PcmChunk chunk{42, 0, 48000, pcm, pcm_count};
auto spans = analyzer.feed(chunk);
```

Each returned span is addressed by the canonical PCM sample position. Feed
chunks must be contiguous and ordered by `sample_begin`; a delayed callback does
not affect the result, while an out-of-order chunk is rejected. Call `reset()`
between utterances. `flush()` is provided for lifecycle symmetry and currently
has no pending output because the baseline has zero look-ahead.

The optional `viseme` field is intentionally empty in v0: amplitude alone is
audio-reactive evidence, not a phoneme claim. Predictive pronunciation and
acoustic-alignment providers can be layered on later without changing the PCM
clock or importing a backend into core.
