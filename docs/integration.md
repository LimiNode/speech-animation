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

The core and this integration layer contain no bridge headers. The queue is
bounded and preallocated at pipeline construction; `push()` never waits for
the analyzer. Its overflow policy is explicit:
`QueuePushResult::Full` rejects the newest chunk and leaves previously accepted
audio untouched; the bridge may retry or apply upstream backpressure. No
callback wall-clock timestamp is observed.

`process_available()` is the consumer-side operation and is the only place that
calls the analyzer. Therefore analyzer work is outside the audio callback and
does not delay playback. Presentation uses the returned sample-addressed spans
against the actual playback sample cursor.

`complete()` and `cancel()` first drain already accepted chunks. They then append
a synthetic zero PCM terminal tail at the next canonical sample position and
emit exactly one terminal receipt. The tail is part of the output PCM timeline,
so mouth activity can fade to silence without changing the played audio. Late
pushes are rejected with `QueuePushResult::RejectedTerminal`.

`SpeechAnimationReceipt::to_json()` emits machine-readable records containing
request ID, input range, output range, sample rate, activity, mouth opening, and
terminal/tail markers. A bridge can write these records as JSONL without making
the generic core aware of its own callback or request types.
