# OnlyObsessionRemained — Reverb DSP

Three self-contained reverb algorithms in portable C++17, plus an offline
verification harness that measures them instead of just listening to them.

No JUCE, no plugin framework, no audio device. The whole library is a couple of
hundred kilobytes of source and builds in seconds.

> Source comments are in Chinese; this README is in English for accessibility.

**English** | [中文](README.zh-CN.md)

## What is here

| Path | Contents |
| --- | --- |
| `Source/dsp/core/` | Building blocks: fractional delay line, allpass, one-pole low/high pass, triangle LFO, soft saturator, octave pitch shifter |
| `Source/dsp/tanks/PlateTank.{h,cpp}` | Plate reverb — Dattorro figure-8 topology |
| `Source/dsp/tanks/RoomTank.{h,cpp}` | Room reverb — 8-line feedback delay network with a Householder mixing matrix |
| `Source/dsp/tanks/SpringTank.{h,cpp}` | Spring reverb — 48-stage allpass dispersion chain inside the feedback loop |
| `offline/PlateOfflineTest.cpp` | Verification harness (see below) |

## Design notes

**Damping is a blend, not a series filter.** In every tank the feedback path
applies `y = x + k * (filtered - x)`, where `k` is the High/Low Damp parameter.
At `k = 0` the path is bit-transparent, so "no damping" means the same thing in
all three algorithms and RT60 values are comparable across them. A fixed 15 Hz
DC blocker sits in every feedback path, because a transparent high-pass would
otherwise let DC accumulate with a loop gain of up to `1/(1-g)`.

**Loop gains are calibrated per delay line** so every line in a network reaches
-60 dB at the same moment. Using one shared gain makes short lines decay first
and leaves the tail dominated by the longest line, which sounds muddy.

**RT60 mapping is identical across the three algorithms**:
`RT60 = 0.25 * 120^decay`. Parameters are normalised 0..1.

**Modulation** offsets delay-line read pointers with slow, mutually
incommensurate triangle LFOs. At `mod = 0` the fractional read degenerates to
an integer read, so the tank is bit-identical to a static one.

**Feedback saturation** is a soft clip with a knee at 0.7 that approaches an
asymptote of 2.0. It is bit-transparent below the knee; each tank uses its own
drive so the knee lands at roughly the same wet-output level.

**Room delays** are eight mutually prime lengths summing to about 0.90 s at
48 kHz, which satisfies the modal density lower bound `sum >= 0.15 * RT60 * fs`
up to roughly 6 s of RT60.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Requirements: CMake 3.16+, a C++17 compiler. On Windows, build from a Visual
Studio developer shell.

## What the harness verifies

`plate_offline_test` runs 22 checks and exits non-zero if any fails:

- impulse response: peak, first arrival, echo density, RT60 by EDC extrapolation
- Decay → RT60 calibration for all three tanks (measured vs nominal ratio)
- monotonicity of RT60 in Decay
- High Damp shortens the high band; Low Damp shortens the low band
- wet/dry loudness at the default setting
- pre-delay: first arrival shifts by exactly the requested number of samples
- stability: no NaN, no self-oscillation at extreme settings
- spring dispersion: low frequencies arrive measurably later than high ones
- pitch shifter: octave-up spectrum, anti-aliasing fold-back level
- saturator: curve properties and THD across input levels
- hot-signal compression stays mild rather than turning into limiting

Typical output ends with a per-section table and `结论：全部检查通过`
("all checks passed").

## License

MIT — see [LICENSE](LICENSE).
