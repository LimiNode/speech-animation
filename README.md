# speech-animation

Engine-agnostic, sample-addressed realtime speech animation cues.

The v0 analyzer is a deterministic causal PCM baseline. It exposes normalized
energy, mouth opening, and speech activity. DSP is updated per sample, while
public spans are emitted on a fixed sample-clock hop (10 ms by default). It
does not import or know about Qwen, PocketTTS, xVibe, Unity, Godot, VRM, or
Live2D.

## v0 usage

```cpp
#include <speech_animation/speech_animation.hpp>

speech_animation::SpeechAnimationAnalyzer analyzer;
speech_animation::PcmChunk chunk{42, 0, 48000, pcm, pcm_count};
auto spans = analyzer.feed(chunk); // complete 10 ms hops
auto final_spans = analyzer.flush(); // optional final partial hop
```

Each returned span is addressed by the canonical PCM sample position. Feed
chunks must be contiguous and ordered by `sample_begin`; a delayed callback does
not affect the result, while an out-of-order chunk is rejected. Hop boundaries
are anchored to the first accepted sample position and then advance by the
configured sample count, never by input chunk boundaries. Call `reset()` between
utterances. `flush()` terminates the current utterance, emits at most one final
partial hop, and is idempotent; calling `feed()` afterward requires `reset()`.

Set `AnalyzerConfig::output_hop_samples` for an exact hop, or use
`output_hop_ms` (rounded once after the sample rate is known). When exact sample
count is set, `output_hop_ms` is ignored and may be zero. The internal DSP
remains causal and has no look-ahead.

Input is normalized floating-point PCM. Finite values outside `[-1, 1]` are
clipped, and non-finite samples are treated as silence. This sanitization is
intentional so that one malformed realtime sample does not abort an utterance.

The optional `viseme` field is intentionally empty in v0: amplitude alone is
audio-reactive evidence, not a phoneme claim. Predictive pronunciation and
acoustic-alignment providers can be layered on later without changing the PCM
clock or importing a backend into core.

To run the chunk-sensitivity benchmark:

```powershell
cmake -S . -B build -DSPEECH_ANIMATION_BUILD_BENCHMARKS=ON
cmake --build build --config Release --target speech_animation_benchmark
build\speech_animation_benchmark.exe
```

The optional `speech_animation::integration` target provides a bridge-neutral
vertical slice. Push owned `SpeechTimingChunk` values into its preallocated
bounded queue, call `process_available()` from a consumer thread, and serialize
the returned `SpeechAnimationReceipt` values with `to_json()`. `complete()` and
`cancel()` drain accepted PCM, append a sample-addressed zero tail, and mark one
terminal receipt; late chunks are rejected.
