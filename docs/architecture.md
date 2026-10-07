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
cross-utterance boundary. DSP state evolves per sample, then public spans are
summarized on a fixed hop anchored to the first accepted sample position. A
complete run is therefore invariant under one large chunk, fixed-size chunks,
irregular chunks, and one-sample chunks; hop boundaries never move with input
chunk boundaries.

`SpeechAnimationSpan::viseme` is empty in v0. Energy and mouth opening carry
`AudioReactive` semantics and must not be presented as a known phoneme. A
future pronunciation provider may add `PredictedPronunciation` spans, while a
later aligner may refine only future spans to `AcousticAligned`; already played
sample ranges remain immutable at the presentation layer.

The fixed-hop public representation is the production-facing v0 shape. The
benchmark compares 1-sample, 80-sample, 10 ms, 40 ms, and 80 ms input chunks;
all produce the same sample-addressed hop timeline. A future adapter may choose
a different hop, but it must remain anchored to the canonical sample clock.
`flush()` emits one final partial hop when the utterance does not end on a hop
boundary, then becomes idempotent.

Progress evidence belongs to the predictive-viseme milestone, where its actual
consumer and required fields can define the contract. The v0 core must not
include Qwen, PocketTTS, xVibe, Unity, Godot, VRM, or Live2D headers.
