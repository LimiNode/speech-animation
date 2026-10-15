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
against the actual playback sample cursor. `PlaybackLockedCueSelector` is the
engine-neutral consumer for that final step: it accepts an epoch/request-scoped
cursor snapshot, returns the cue covering the current sample, and returns a
neutral selection when the cursor is unavailable, stale, before/after the
available ranges, or already past a terminal fade. It never uses callback wall
clock timestamps or interpolates across a gap.

Prepared pronunciation predictions are appended separately with
`append_prediction(playback_epoch, request_id, sample_rate, PredictedVisemeCue)`.
The epoch is mandatory so delayed producers from an earlier playback cannot
inject cues after `reset()`. They are
validated and kept on their own bounded sample-addressed track, so they cannot
change the PCM receipt timeline. On a playback tick, a prediction covering the
cursor is exposed as `PlaybackAnimationSelection::viseme`; if no prediction is
available, the selection contains no viseme and the presentation adapter uses
the audio-reactive `mouth_open`/activity fallback.

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
terminal/fade markers. A bridge can write these records as JSONL without making
the generic core aware of its own callback or request types.

`PlaybackCursorSnapshot` is deliberately supplied by the playback sink. A
WinMM/WaveOut adapter may populate it from `waveOutGetPosition(TIME_SAMPLES)`;
the generic library does not depend on WinMM or any other audio API.
