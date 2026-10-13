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
`flush()` is the terminal boundary for the current utterance: it emits one
final partial hop when needed, then becomes idempotent. A subsequent `feed()`
is rejected until `reset()`, so an intermediate flush cannot shift the hop
grid.

The predictive-viseme contract now lives in `predictive.hpp`: a pronunciation
provider supplies an ordered viseme sequence and relative duration weights,
then `PredictiveVisemePlanner` allocates contiguous sample-addressed cues and
can annotate analyzer spans by overlap. It deliberately does not parse text,
infer phonemes, add look-ahead, or include Qwen, PocketTTS, xVibe, Unity, Godot,
VRM, or Live2D headers. Acoustic alignment remains a later evidence upgrade.

The optional application integration layer in `integration/` maps neutral
`SpeechTimingChunk` values into `PcmChunk`, drains them through a bounded
non-blocking queue, and emits JSON-serializable receipts. It is deliberately
outside the core target so a TTS bridge can depend on it without making the
analyzer depend on that bridge. Completion and cancellation append a synthetic
fade receipt at the next sample position and mark exactly one terminal receipt;
no synthetic PCM is inserted into the playback timeline.

## Predictive pronunciation boundary

`core/include/speech_animation/predictive.hpp` defines the backend-neutral
boundary for the next milestone. A pronunciation provider emits
`PronunciationUnit` symbols and confidence; `PronunciationVisemeMapper` maps
those symbols to the small semantic `Viseme` taxonomy. The mapper is
conservative: unsupported symbols become low-confidence `Neutral`, and no
audio amplitude is treated as phoneme evidence.

ASCII normalization changes only `A`-`Z`; all other bytes, including UTF-8,
are preserved byte-for-byte. An empty pronunciation symbol is unknown rather
than silence. Explicit pause markers map to low-confidence `Closed`, while
Cyrillic spelling-level approximations use reduced confidence where context
can change the underlying phonetics; `й` is a low-confidence `Neutral` glide.

`PredictiveVisemePlanner` then consumes the resulting
`PredictedVisemeSpec` sequence together with a `SpeechContext` containing the
known PCM duration. It emits contiguous `PredictedVisemeCue` records on the
canonical sample clock, with `PredictedPronunciation` evidence. The output is
deterministic for a given pronunciation sequence and duration, independent of
callback timing or input chunking.

This is intentionally a full-duration vertical slice. Streaming placement
with an unknown final duration, PCM-driven correction, and acoustic alignment
remain later stages. A presentation consumer must treat these cues as
tentative and fall back to the audio-reactive signal when no reliable
prediction is available.
