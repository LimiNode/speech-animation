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
`cancel()` drain accepted PCM, then emit one explicit terminal-fade receipt
anchored at the next sample position; no synthetic PCM is inserted into the
playback timeline. Call `begin()` when a request may terminate before its first
PCM chunk; late chunks are rejected.

## Predictive pronunciation v0

The core also contains a deliberately small pronunciation-to-viseme vertical
slice. An application-owned pronunciation provider supplies neutral
`PronunciationUnit` values; `PronunciationVisemeMapper` converts them to
semantic `PredictedVisemeSpec` values, and `PredictiveVisemePlanner` places the
sequence on a known PCM duration:

```cpp
#include <speech_animation/predictive.hpp>

const auto specs = speech_animation::PronunciationVisemeMapper::map_sequence(units);
speech_animation::SpeechContext context{request_id, first_sample, sample_rate,
                                        total_sample_count};
const auto cues = speech_animation::PredictiveVisemePlanner{}.plan(context, specs);
```

Every cue is contiguous and sample-addressed. Its `VisemeEstimate::evidence` is
always `PredictedPronunciation`; it is never presented as acoustic confirmation.
Unknown pronunciation symbols become low-confidence `Neutral` cues, allowing a
presentation layer to fall back to the audio-reactive mouth signal. The planner
requires the complete PCM duration in this milestone. Online placement while a
streaming backend has unknown final duration is intentionally a separate API
milestone and does not add playback delay here.
