# Live audio-driven lip-sync

`integration::LiveAudioReactiveSession` is the capture-side, audio-only
vertical slice for user and remote-speaker avatars. It accepts normalized mono
PCM on a monotonic capture sample clock and emits the same
`SpeechAnimationSpan` representation used by generated speech:

```text
capture adapter
  -> LivePcmChunk(source_id, source_epoch, first_sample, sample_rate, PCM)
  -> bounded SPSC queue
  -> SpeechAnimationAnalyzer
  -> LiveAudioAnimationBatch(source_id, source_epoch, spans)
```

The session does not import TTS, STT, pronunciation, playback, renderer, or
operating-system capture APIs. A full queue returns `QueuePushResult::Full`
without waiting; the capture owner may retry or apply its own backpressure.
Only the consumer calls the analyzer. `reset()` is lifecycle-owner controlled,
while source IDs, source epochs, and canonical sample positions fence delayed or
mixed input. A new unique source epoch is required even when a device reuses the
same source ID and sample clock.

Mute is snapshotted when a chunk is admitted, so a chunk captured while muted
cannot animate the mouth merely because processing happens after unmute. Mute
is represented by zero PCM on the same capture clock and the emitted spans are
explicitly forced to neutral activity, energy, and mouth opening even when
release smoothing retains pre-mute state. This preserves sample continuity
while producing neutral spans. `stop()` drains accepted
audio and flushes one final partial hop; later chunks are rejected until a
new `reset()` starts a source epoch. Session restart, device replacement, and
network stream replacement therefore do not leak adaptive DSP state.

`process_available()` returns a `LiveAudioAnimationBatch`, not a bare vector.
The batch carries `source_id` and `source_epoch` next to its generic spans, so
presentation code can reject delayed output from an earlier capture restart
even when sample coordinates are reused.

`reset()` always establishes the first sample position. If `sample_rate` is
zero, only the rate is learned from the first chunk; the reset-provided sample
clock remains fenced and cannot be replaced by that chunk.

`max_pcm_chunk_samples` bounds the memory of each admitted chunk independently
of the queue slot count. Oversized chunks are rejected before entering the ring.
`reset()` and `process_available()` are serialized by the consumer lifecycle
guard, so reset cannot clear analyzer state while a drain is active.

## Capture and playback clocks

For a local user avatar, a presentation adapter can select emitted spans against
the display/capture cursor; no microphone playback is needed. For a remote
avatar, the same spans should be selected against the actual post-jitter-buffer
playback cursor. Packet arrival time is never a semantic clock. If the cursor
is unavailable, stale, muted, or belongs to another source epoch, the adapter
must return neutral articulation.

The baseline session is deliberately production-safe and RMS/energy based. The
experimental amplitude and spectral estimators remain optional research
composers that can consume the same `LivePcmChunk` ranges; neither is made the
production default by this slice. Their soft weights retain `AudioReactive`
provenance and must not be interpreted as phoneme recognition.

MFCC/template profiles, device APIs, VRM mappings, and renderer smoothing are
separate follow-up work. No microphone samples are persisted by the session.

For a local CPU/latency baseline, build with
`SPEECH_ANIMATION_BUILD_BENCHMARKS=ON` and run
`speech_animation_live_benchmark`. It reports processing time for five seconds
of 48 kHz PCM and a realtime factor; the analyzer emits only completed causal
hops, so the hop size is the bounded analysis-to-cue availability interval,
not added audio playback latency.
