# Application integration vertical slice

`integration::SpeechAnimationPipeline` is the neutral application-layer seam
between a TTS bridge and the analyzer. A bridge copies or moves its own audio
callback payload into `SpeechTimingChunk`:

```text
bridge callback
  → SpeechTimingChunk(request_id, first_sample, sample_count, sample_rate, PCM)
  → bounded queue
  → SpeechAnimationAnalyzer
  → SpeechAnimationReceipt JSON
```

The core and this integration layer contain no bridge headers. The queue is a
preallocated single-producer/single-consumer ring. `push()` never waits for
the analyzer and uses acquire/release publication; its overflow policy is:
`QueuePushResult::Full` rejects the newest chunk and leaves previously accepted
audio untouched; the bridge may retry or apply upstream backpressure. No
callback wall-clock timestamp is observed.

`process_available()` is the consumer-side operation and is the only place that
calls the analyzer. Therefore analyzer work is outside the audio callback and
does not delay playback. Presentation uses the returned sample-addressed spans
against the actual playback sample cursor.

`complete()` and `cancel()` first drain already accepted chunks. They then emit
exactly one `TerminalFade` receipt anchored at the next canonical sample
position. This receipt has `output_sample_count = 0` because no synthetic PCM is
added to the playback timeline; `terminal_fade_sample_count` tells the
presentation layer how long to fade the animation. Late pushes are rejected
with `QueuePushResult::RejectedTerminal`.

Call `begin()` before the producer starts when a request may complete or cancel
before its first PCM chunk. That produces a request-scoped terminal receipt with
zero actual audio output.

`SpeechAnimationReceipt::to_json()` emits machine-readable records containing
request ID, input range, output range, sample rate, activity, mouth opening, and
terminal/tail markers. A bridge can write these records as JSONL without making
the generic core aware of its own callback or request types.
