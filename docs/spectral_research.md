# Spectral-assisted audio-reactive viseme research

This document describes the opt-in research target added for the audio-reactive
shape experiment. It is deliberately separate from `SpeechAnimationAnalyzer`:
the production RMS baseline and its output contract are unchanged.

## Hypotheses

- A: the existing RMS/adaptive-normalization analyzer and `mouth_open`.
- B: an amplitude-only shape heuristic. It uses the smoothed mouth envelope to
  choose a small set of broad shapes, similar in spirit to an amplitude-driven
  lip-sync system.
- C: a spectral-assisted shape heuristic. It estimates relative energy in three
  physical frequency bands and maps those ratios to soft A/E/I/O/U weights.

The C variant uses a small Goertzel bank at frequencies derived from configured
Hz bands. It has no FFT or neural dependency. The default trailing window is
30 ms and the output hop is 10 ms. Both can be overridden with exact sample
counts. Frequencies are converted from Hz at the actual input sample rate, so
the implementation does not copy sample-rate-dependent FFT-bin numbers from
other projects.

Neither B nor C recognizes a phoneme. A present estimate always carries
`AnimationEvidence::AudioReactive`, and confidence is capped by
`maximum_confidence` (0.60 by default). Silence, weak energy, and low spectral
contrast leave `viseme` empty while retaining the generic energy/activity data.

The analyzer is causal: a cue is emitted only after its trailing analysis
window consists of observed samples. It stores at most one configured window,
rejects gaps/overlaps, ignores callback arrival time, and emits absolute
sample-addressed ranges. `flush()` emits an observed tail using the latest
available frame; it never invents future PCM.

## Reproducible experiment

Configure with `-DSPEECH_ANIMATION_BUILD_RESEARCH=ON`, then build and run:

```text
cmake -S . -B build -DSPEECH_ANIMATION_BUILD_TESTS=ON \
  -DSPEECH_ANIMATION_BUILD_BENCHMARKS=ON \
  -DSPEECH_ANIMATION_BUILD_RESEARCH=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
build/speech_animation_spectral_benchmark
```

The benchmark uses five seconds of deterministic 48 kHz synthetic voiced
segments (440, 600, 800, 1200, and 2500 Hz), compares 80/480/1920-sample input
chunks, and reports runtime, confident cues, viseme switches, and mean weight
delta. One local Release run produced:

| Variant | Chunk | ms / 5 s | Realtime factor | Cues | Confident | Switches | Mean Δweights |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| A baseline | 80 | 8.930 | 560x | 500 | 0 | 0 | 0.000 |
| B amplitude | 80 | 4.708 | 1062x | 499 | 499 | 0 | 0.000 |
| C spectral | 80 | 26.529 | 188x | 499 | 281 | 11 | 0.048 |
| A baseline | 480 | 8.947 | 559x | 500 | 0 | 0 | 0.000 |
| B amplitude | 480 | 4.116 | 1215x | 499 | 499 | 0 | 0.000 |
| C spectral | 480 | 26.628 | 188x | 499 | 281 | 11 | 0.048 |
| A baseline | 1920 | 8.533 | 586x | 500 | 0 | 0 | 0.000 |
| B amplitude | 1920 | 5.297 | 944x | 499 | 499 | 0 | 0.000 |
| C spectral | 1920 | 27.441 | 182x | 499 | 281 | 11 | 0.048 |

These numbers are implementation evidence, not a perceptual quality claim;
they depend on the machine and synthetic fixture. The tests additionally cover
one-chunk versus irregular partitioning, causal availability, silence, 16/24/48
kHz operation, spectral response to different tones, bounded evidence, and
configuration validation.

## Interpretation and next step

The experiment demonstrates a deterministic, inexpensive and sample-rate-aware
fallback that produces visibly different hypotheses from a single scalar
`mouth_open`. It does not demonstrate vowel accuracy: fixed spectral bands are
affected by speaker, microphone, consonants, noise, and room response. No VRM
renderer harness or annotated Russian/English corpus is part of this research
PR, so there is not yet evidence to change production behavior.

Recommendation: keep B/C opt-in and experimental. Evaluate them on a fixed
real-speech corpus and a reproducible VRM renderer before considering a minimal
production opt-in. If pronunciation prediction is available, it remains the
stronger source; these audio-reactive estimates are suitable as a fallback and
must not be relabeled `PredictedPronunciation` or `AcousticAligned`.
