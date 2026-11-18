# OnlyObsessionRemained

A reverb plugin — three mutually independent reverb algorithms (plate, room,
spring) plus shimmer, built on JUCE 8 and shipping as VST3 and a standalone app.
The interface is entirely vector-drawn and driven by a skin manifest.

> Source comments are in Chinese; this is the English translation. The Chinese
> README is the default: [README.md](README.md).

**English** | [中文](README.md)

## Download

Prebuilt Windows x64 binaries (standalone `.exe` + `.vst3`) are attached to the
[Releases](../../releases) page. Unpack the zip, run the `.exe`, or copy the
`.vst3` into `C:\Program Files\Common Files\VST3`.

## Parameters

| Parameter | Range | Notes |
| --- | --- | --- |
| Type | Plate / Room / Spring | Crossfaded over 120 ms; the outgoing tank keeps ringing while it fades |
| Mix | 0 – 100 % | Wet/dry |
| Decay | 0 – 100 % | `RT60 = 0.25 * 120^decay` seconds, identical across all three tanks |
| Pre-delay | 0 – 200 ms | Fractional-sample accurate |
| Mod | 0 – 100 % | Slow, mutually incommensurate triangle LFOs on the read pointers |
| High Damp | 0 – 100 % | Blended low-pass in the feedback path |
| Low Damp | 0 – 100 % | Blended high-pass in the feedback path |
| Shimmer | 0 – 100 % | Octave-up pitch shifter inside the feedback loop |

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

Requirements: CMake 3.22+, a C++17 compiler, and network access on the first
configure — JUCE 8.0.15 is fetched with `FetchContent` and pinned to that exact
tag. On Windows, build from a Visual Studio developer shell.

Artifacts (Ninja/Release):

| Target | Path |
| --- | --- |
| Standalone app | `build/OnlyObsessionRemained_artefacts/Release/Standalone/OnlyObsessionRemained.exe` |
| VST3 | `build/OnlyObsessionRemained_artefacts/Release/VST3/OnlyObsessionRemained.vst3` |

With the Visual Studio generator the same two live under
`build/OnlyObsessionRemained_artefacts/Release/`.

`-DSKIN_NAME=<dir>` selects which `skins/<dir>` is packed into the binary; the
default is `default`.

## What is here

| Path | Contents |
| --- | --- |
| `Source/PluginProcessor.*` | Audio processor: parameter state, block splitting, type crossfade |
| `Source/PluginEditor.*` | Editor shell and parameter bindings |
| `Source/params/` | Single source of truth for the eight parameters |
| `Source/presets/` | Factory presets and preset manager |
| `Source/dsp/ReverbEngine.*` | Chooses the active tank, smoothing, shimmer routing |
| `Source/dsp/tanks/` | The three reverb algorithms |
| `Source/dsp/core/` | Building blocks: fractional delay line, allpass, one-pole low/high pass, triangle LFO, soft saturator, octave pitch shifter |
| `Source/ui/` | Vector HUD, skin loader, skin designer, GLSL background, window shaping |
| `skins/default/skin.json` | Manifest of the built-in look (colours, fonts, rects, labels) |
| `tools/skin_pack.cpp` | Packs a skin directory into generated C++ at configure time |
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

**Type switching crossfades** instead of resetting: both tanks run for 120 ms
with equal-power gains, and the outgoing tank is only reset once it is silent.
A switch requested mid-fade is queued rather than dropped.

**Feedback saturation** is a soft clip with a knee at 0.7 that approaches an
asymptote of 2.0. It is bit-transparent below the knee; each tank uses its own
drive so the knee lands at roughly the same wet-output level.

**Room delays** are eight mutually prime lengths summing to about 0.90 s at
48 kHz, which satisfies the modal density lower bound `sum >= 0.15 * RT60 * fs`
up to roughly 6 s of RT60.

**The UI only draws.** Layout is a manifest plus per-block transforms stored in
`hud-layout.json` next to the plugin state, with a version field. The current
layout ships as the compiled-in default, so a fresh install looks the same
everywhere. If the GPU cannot compile the background shader, the editor falls
back to a CPU-drawn path.

## What the harness verifies

`plate_offline_test` runs 22 checks and exits non-zero if any fails. It links
only the three tanks, no JUCE, no host:

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
