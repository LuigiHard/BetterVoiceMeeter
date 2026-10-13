# Roadmap

## App Mixer

Prototype of all three concepts: https://claude.ai/artifact/BfdM5eLQVdRHKU7zpwGcmX

- **A · App faders** (done, v0.3.x): per-app volume, mute and meters through Windows' per-app session volume.
  `Source/AppSessions.*` (session discovery, icons, levels) + `Source/UI/AppMixer.*` (panel).
- **B · Apps as inputs** (planned, optional mode): each app becomes a full input strip (EQ, dynamics, VST3, A1–B2
  routing). Capture each app with Windows process-loopback capture
  (`ActivateAudioInterfaceAsync` + `AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`, Windows 10 2004+), feed it through
  a `DriftFifo` into a strip like the virtual inputs, and silence the app's original output (route it to a silent
  endpoint) so it isn't heard twice. Adds ~10–20 ms to app audio; the mic path stays direct.
  Some games' anti-cheat may block capture: keep those on A-style control.
- **C · Mix groups** (planned, optional mode, built on B): Game / Chat / Music / Browser groups, each with a
  "you" level (A buses) and a "stream" level (B buses). Apps are assigned by their exe path (the key `AppSessions`
  already uses), so assignments survive restarts. A group can expand into B-style per-app strips.

The App Mixer panel should offer A as the default, with B and C as opt-in modes once built.
