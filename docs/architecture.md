# Architecture notes

The core has one clock: `(utterance_id, sample_begin, sample_count,
sample_rate)` from PCM. It never observes callback arrival time. `feed()`
requires contiguous sample positions, so a reordered callback is rejected
instead of silently changing animation timing. An adapter can queue and reorder
chunks before calling the analyzer if its transport permits reordering.

The v0 path is deliberately causal:

```text
PCM sample → one-pole energy envelope → adaptive normalization
           → silence/activity + mouth opening
```

There is no look-ahead buffer and no background thread. State is owned by one
`SpeechAnimationAnalyzer` instance, and `reset()` is the explicit
cross-utterance boundary. Per-sample spans make a complete run invariant under
one large chunk, fixed-size chunks, irregular chunks, and one-sample chunks;
the only accepted difference between runs is normal floating-point arithmetic
on the same ordered samples (which is identical for this implementation).

`SpeechAnimationSpan::viseme` is empty in v0. Energy and mouth opening carry
`AudioReactive` semantics and must not be presented as a known phoneme. A
future pronunciation provider may add `PredictedPronunciation` spans, while a
later aligner may refine only future spans to `AcousticAligned`; already played
sample ranges remain immutable at the presentation layer.

Backend bridges should convert their own progress markers to the neutral
`SpeechGenerationProgress` type. The core must not include Qwen, PocketTTS,
xVibe, Unity, Godot, VRM, or Live2D headers.
