# Spectral corpus runner

The optional `speech_animation_spectral_corpus` tool runs the experimental
audio-reactive estimator over a WAV file and writes newline-delimited JSON
receipts. It is deliberately separate from the production analyzer and does
not claim phoneme recognition.

Build it with:

```text
cmake -S . -B build -DSPEECH_ANIMATION_BUILD_RESEARCH=ON \
  -DSPEECH_ANIMATION_BUILD_TOOLS=ON
cmake --build build --config Release --target speech_animation_spectral_corpus
```

Run both variants independently:

```text
build/speech_animation_spectral_corpus --input utterance.wav --mode amplitude > amplitude.jsonl
build/speech_animation_spectral_corpus --input utterance.wav --mode spectral > spectral.jsonl
```

The first record contains decoded input metadata (`sample_rate`, frame count,
and channel count). Each following `audio_span` record is sample-addressed and
contains `energy`, `mouth_open`, `activity`, soft A/E/I/O/U weights, and an
optional low-confidence audio-reactive viseme hypothesis. The tool accepts PCM
8/16/24/32-bit and IEEE-float 32-bit WAV files and downmixes multichannel input
to mono. The supported WAV format codes are classic PCM (`1`) and IEEE float
(`3` with 32-bit samples); `WAVE_FORMAT_EXTENSIBLE` is intentionally rejected
until its channel-mask/subformat semantics are specified. RIFF container size,
block alignment, and frame alignment are validated. The CI corpus smoke test
covers mono and stereo 8/16/24/32-bit PCM, float32, malformed headers, and
unsupported format codes. No callback timing is used.

For a TTS validation receipt, keep the JSONL together with the source text,
language, model identity, seed, WAV SHA-256, analyzer configuration, and tool
version. Human listening is a complementary check; non-zero soft weights are
not pronunciation evidence.
