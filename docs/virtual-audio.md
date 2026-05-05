# Virtual Audio Sinks — CrossPad as a System Mixer

CrossPad-PC can act like **VoiceMeeter Banana** on Windows or **Loopback** on
macOS: it exposes two virtual "speakers" that other applications and DAWs can
route their audio into. CrossPad then mixes those streams with its built-in
synth and routes everything to the physical outputs.

> Status: **Linux works out of the box.** Windows and macOS detection are
> planned for a follow-up release — see [Roadmap](#roadmap).

## What You Get

When CrossPad-PC starts, it creates two OS-visible outputs:

- **CrossPad virtual IN#1** — fed into the mixer as IN1
- **CrossPad virtual IN#2** — fed into the mixer as IN2

Any application (Spotify, Discord, Firefox, Reaper, Ardour…) that can pick an
audio output will see these as regular "speakers". The audio you send to them
becomes the input of the CrossPad mixer.

```
  Spotify ──▶ CrossPad virtual IN#1 ──▶ Mixer IN1 ─┐
                                                    ├─▶ Mixer ──▶ OUT1 ──▶ physical speakers
  Reaper  ──▶ CrossPad virtual IN#2 ──▶ Mixer IN2 ─┤
                                                    └─▶ OUT2 ──▶ headphones
  Synth / MIDI pads ───────────────────▶ Mixer Synth
```

End-to-end latency target: **~15 ms** with default settings.

## Linux (PulseAudio / PipeWire)

**Nothing to install.** CrossPad shells out to `pactl` to create two
`module-null-sink`s at launch and unloads them at exit.

### Quick verification

```bash
# 1. Launch CrossPad
./bin/CrossPad

# 2. In another terminal, confirm the sinks are live
pactl list sinks short | grep crosspad_vin
#   → 42  crosspad_vin1  ...
#   → 43  crosspad_vin2  ...

# 3. Send some audio to a virtual sink — CrossPad should capture it
mpv --audio-device=pulse/crosspad_vin1 some-music.mp3
```

Alternatively, open `pavucontrol` → "Playback Devices": a running player will
appear and you can drop its output to **CrossPad virtual IN#1** or **#2** on
the fly.

### Requirements

- A running PulseAudio **or** PipeWire session (standard on all modern
  distros — Ubuntu 22.04+, Fedora 35+, Arch, etc.)
- `pactl` on `$PATH` — provided by `pulseaudio-utils` on Debian/Ubuntu or
  by `pipewire-pulse` when using PipeWire.

### Clean shutdown

CrossPad unloads its sinks on normal exit, window close, SIGINT, and SIGTERM.
After `kill -9`, the orphaned sinks are cleaned up on the next launch.

### Disabling the feature

```bash
cmake -B build -DUSE_VIRTUAL_AUDIO=OFF && cmake --build build
```

With the feature off, IN1/IN2 revert to picking real microphones from the
saved device preferences.

## Windows / macOS — upcoming

CrossPad will auto-detect the following third-party virtual cables and map
them to IN#1/IN#2 (no driver ships with CrossPad):

- **Windows:** [VB-CABLE A + B](https://vb-audio.com/Cable/)
- **macOS:** [BlackHole 2ch](https://existential.audio/blackhole/)

Until then, you can manually pick one of those devices as IN1/IN2 in the
CrossPad audio settings once installed.

## Troubleshooting (Linux)

| Symptom | Likely cause | Fix |
|---|---|---|
| `pactl / PulseAudio not available — skipping` in logs | No audio daemon in the user session | `systemctl --user start pipewire pipewire-pulse` or install `pulseaudio` |
| Sinks appear but CrossPad doesn't capture | RtAudio built without PulseAudio backend | Rebuild with `-DUSE_VIRTUAL_AUDIO=ON` on a host that has `libpulse-dev` installed |
| Sinks don't get removed after crash | Hard kill (`SIGKILL`) | Next CrossPad launch auto-cleans any `crosspad_vin*` modules |
| Choppy audio | PipeWire quantum too low for your hardware | `pw-metadata -n settings 0 clock.force-quantum 512` |

## Roadmap

- Phase 2 — Windows VB-CABLE auto-detection + install prompt
- Phase 3 — macOS BlackHole auto-detection + install prompt
- Phase 4 — System tray with per-channel mute / volume (background operation)
