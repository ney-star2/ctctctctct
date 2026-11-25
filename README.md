# Boom Bap Master

A one-plugin finishing chain for 90s hip-hop beats: 12-bit / 26 kHz sampler grit, gentle glue, a dusty top end,
slow analogue-style drift, and a loudness stage that lands on **-14 LUFS** with a true-peak limiter at **-1 dBTP**.
VST3 for FL Studio (Windows and macOS).

## Get the plugin (GitHub Actions)

1. Create a GitHub repo and push this folder to it.
2. Open the **Actions** tab. The `Build VST3` workflow runs on every push (about 10 min the first time).
3. Open the finished run and download **BoomBapMaster-Windows-VST3** (or macOS) from *Artifacts*.
4. Optional: push a tag like `v1.0.0` and the zips are attached to a GitHub Release.

The workflow first runs the DSP tests (loudness meter, true-peak limiter, sample-rate and extreme-setting checks).
If they fail, no plugin is built.

## Install in FL Studio (Windows)

1. Unzip and copy the **Boom Bap Master.vst3** folder to `C:\Program Files\Common Files\VST3\`.
2. FL Studio: *Options > Manage plugins > Find installed plugins* (Verify plugins).
3. Put it on the **Master** mixer track as the **last** plugin.

macOS: copy to `/Library/Audio/Plug-Ins/VST3/`. The build is unsigned, so run
`xattr -cr "/Library/Audio/Plug-Ins/VST3/Boom Bap Master.vst3"` once.

## Signal chain (in order)

| Stage | What it does |
|---|---|
| Input trim | Gain before the chain |
| Sub cut | 25 Hz high-pass, keeps inaudible rumble out of your headroom |
| Saturation | Oversample-free ADAA tanh, a few percent, with an On button |
| 12-bit / 26 kHz | SP-1200-style: 6th-order band-limit, 26.04 kHz sample-and-hold, 12-bit quantise, with a Mix control |
| Glue compressor | 1.2:1 to 2.4:1, soft knee, 30 ms attack, auto release, gain reduction capped at about 2-4 dB |
| 90s tone | Gentle high-shelf down plus a touch of low-mid, one knob |
| Drift | Slow filtered-noise wow (about 1 Hz) plus tiny flutter (about 9 Hz), under 2 cents. Not a sine LFO. |
| Mono bass | Side channel high-passed at 110 Hz (adjustable), so the low end is always centred |
| Level | Manual, Auto (rides to the target LUFS) or Locked |
| Crackle | Sparse vinyl clicks and a very quiet noise floor, off by default |
| True-peak limiter | 8x true-peak detection, 2 ms look-ahead, final stage, ceiling -1 dBTP by default |

Latency is about 3 ms and is reported to FL Studio, so it is compensated automatically.

## Getting exactly -14 LUFS / -1 dBTP

1. Set Level to **Auto** and play the whole beat once. The input and output LUFS meters fill in.
2. Press **LOCK GAIN**. The gain it found is frozen (Locked mode).
3. Render. Every render is now identical and lands on the target.

Reset the meters with **RESET METERS** if you change the arrangement.

## Suggested starting levels (before the plugin)

Drums peak about -9 dB, sample about -12 dB, bass about -15 dB, hats about -19 dB.

## Build locally

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

DSP tests only (no JUCE needed):

```
g++ -std=c++17 -O2 -ISource Tests/DspTest.cpp -o dsptest && ./dsptest [sampleRate]
```

## Notes

- The tests check measurable things (loudness, true peak, stability). How it *sounds* is for your ears: the amounts of
  saturation, 12-bit mix, tone and drift are all taste controls.
- JUCE is licensed under AGPLv3 or a commercial/personal licence. Using the plugin yourself is fine. If you share or sell
  the plugin itself, check the JUCE licence terms first.
