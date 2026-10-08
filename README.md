# BetterVoiceMeeter

A Voicemeeter-style virtual mixer for Focusrite interfaces (Windows), built with C++ and JUCE 9.
It runs directly on the **Focusrite USB ASIO** driver, so a signal going hardware in → processing → hardware out costs only one driver buffer (about 9 ms round trip at 48 kHz / 64 samples).

## Download
Grab `BetterVoiceMeeter-v0.1.0-win64.zip` from the [Releases](../../releases) page, extract it, and run `BetterVoiceMeeter.exe`.
Requires a Focusrite interface with the Focusrite USB ASIO driver installed. Optional: [VB-Cable](https://vb-audio.com/Cable/) for the virtual outputs.

## Features
- **Input strips** (one per interface channel, auto-assigned; Focusrite *Loopback 1+2* becomes one stereo strip)
  - Voicemeeter-style XY pad: X = pan, Y = tone (warm ↔ bright tilt EQ)
  - Front-panel Gate and Comp knobs, Reverb/Delay sends, A1–A3 / B1–B2 bus routing, fader + meter, Solo/Mute
  - **FX** button opens the full channel strip: trim, polarity, low-cut, drive/saturation, gate (threshold/range/attack/hold/release),
    3-band parametric EQ with a live curve, compressor (threshold/ratio/attack/release/knee/makeup/auto gain), de-esser,
    plus **presets**: Vocal, Podcast/Stream, Electric Guitar (DI), Acoustic Guitar, Bass (DI), Keys/Synth
- **2 virtual input strips:** capture any Windows recording device through WASAPI
- **FX returns:** Reverb and stereo ping-pong Delay, each with its own bus routing
- **Master section**
  - A1–A3: Focusrite output pairs
  - B1–B2: any Windows playback device (e.g. VB-Cable "CABLE Input", so Discord/OBS can use "CABLE Output" as a mic)
  - Each bus has a 3-band EQ, limiter, mono, mute, and fader
- Virtual I/O is clock-drift compensated (adaptive resampling FIFO)
- Settings auto-save to `%APPDATA%\BetterVoiceMeeter\settings.xml`; mixes save/load as `.bvm` presets

## Build
Requires CMake ≥ 3.22 and Visual Studio 2019+ (C++ workload).
```
git clone --depth 1 --branch 9.0.3 https://github.com/juce-framework/JUCE.git external/JUCE
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release
```
Output: `build/BetterVoiceMeeter_artefacts/Release/BetterVoiceMeeter.exe`

## Notes
- Preamp gain, 48V phantom power, INST, and AIR are analog settings on the interface. Set them with Focusrite Control 2; this app's **TRIM** is digital.
- Turn off Direct Monitor in Focusrite Control so you don't hear your input twice.
- Device and driver log: `%APPDATA%\BetterVoiceMeeter\BetterVoiceMeeter.log`

## License
AGPL-3.0. Built with [JUCE](https://juce.com) (AGPLv3) and the Steinberg ASIO headers bundled with JUCE.
ASIO is a trademark and software of Steinberg Media Technologies GmbH.
