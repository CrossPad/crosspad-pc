# Virtual Audio Sinks — CrossPad as a System Mixer

CrossPad-PC can act like **VoiceMeeter Banana** on Windows or **Loopback** on
macOS: it exposes virtual audio endpoints that other applications and DAWs can
route into and out of. CrossPad mixes those streams with its built-in synth
and routes the result to the physical outputs.

> Status: **Linux works out of the box** via a native PipeWire backend.
> Windows and macOS are planned — see [Fallback matrix](#fallback-matrix).

## What You Get

```
  Spotify ──▶ crosspad_vin1 (Sink) ──▶ Mixer IN1 ─┐
                                                    ├─▶ Mixer ──▶ OUT1 ──▶ physical speakers ──▶ crosspad_out (Source) ──▶ OBS/DAW mic input
  Reaper  ──▶ crosspad_vin2 (Sink) ──▶ Mixer IN2 ─┤
                                                    └─▶ OUT2 ──▶ headphones
  Synth / MIDI pads ───────────────────▶ Mixer Synth
```

- **`crosspad_vin1` / `crosspad_vin2`** — `Audio/Sink` pw_streams ("CrossPad
  IN#1"/"IN#2"). Any app that can pick an audio output sees these as regular
  speakers; audio sent to them becomes Mixer IN1/IN2.
- **`crosspad_out`** — an `Audio/Source/Virtual` pw_stream ("CrossPad Out").
  Fed by an aux tap on the mixer's OUT1 bus (post-master int16 mirror), it
  makes CrossPad show up as a microphone in OBS/DAWs — no loopback device
  needed.

All three are S16/48k/stereo; the PipeWire adapter handles format/rate
conversion for clients that use something else. Monitor ports on the vin
streams come for free (PipeWire adds them automatically for any `Audio/Sink`
node), which is what lets external tools like `qpwgraph` tap the same signal
today (see [docs/audio-plugins.md](audio-plugins.md)).

End-to-end latency target: **~15 ms** with default settings.

## Architecture (Linux, native PipeWire)

`src/audio/pipewire/` hosts the backend:

- **`PwContext`** — process-wide singleton owning the one `pw_thread_loop` +
  `pw_core` connection. `init()` is idempotent and remembers a failed attempt
  rather than retrying every call site; `shutdown()` is safe to call twice.
- **`PwVirtualSinkCapture`** — one `Audio/Sink` stream per virtual input
  (`IAudioInput` impl). `onProcess`/`read` touch only the ring buffer and
  atomics — no allocation, locking, or logging on the RT thread.
- **`PwVirtualSinkManager`** — `IVirtualSinkManager` impl wrapping two
  `PwVirtualSinkCapture` instances (`crosspad_vin1`/`crosspad_vin2`).
- **`PwVirtualSource`** — the `Audio/Source/Virtual` stream backing
  `crosspad_out`; `pushSamples()` is called from the audio thread via the
  mixer's aux-stream tap.
- **`PwDefaultSinkGuard`** — reads/writes WirePlumber's `default` metadata
  object to make `crosspad_vin1` the system default sink while CrossPad is
  running, and to restore the previous default afterwards.

### Default-sink takeover, restore, and crash recovery

On startup, if the `pw_takeover_default` preference is true (default **on**)
and the native backend is active:

1. `PwDefaultSinkGuard::takeover("crosspad_vin1")` reads the current
   `default.configured.audio.sink` value, saves it, and sets the key to
   `crosspad_vin1` — WirePlumber applies the change.
2. The saved value is persisted immediately to `device_preferences.json` as
   `pw_prev_default_sink`, *before* any teardown path can run.
3. On clean shutdown, `restore()` writes the saved sink back (or clears the
   key if none was saved); `pw_prev_default_sink` is cleared and re-saved —
   a normal exit leaves no trace.
4. **Crash recovery:** if CrossPad is killed (`SIGKILL`) before step 3 runs,
   `pw_prev_default_sink` stays non-empty on disk. The *next* launch feeds
   that value into `setPreviousSink()` before `takeover()` runs, so the sink
   from before CrossPad ever touched the system — not its own leftover
   default — is what eventually gets restored.

Toggling `pw_takeover_default` off keeps the vin sinks created and
capturable, but CrossPad never touches the system default. There's no
settings-screen UI for this yet (lives in `crosspad-gui`, out of scope here)
— hand-edit the bool in the device-preferences JSON at
`pc_platform_get_profile_dir()` + `/device_preferences.json` (see
`getDevicePrefsPath()` in `src/crosspad_app.cpp`).

### Shutdown order

`crosspad_app_shutdown()` runs PipeWire teardown before the generic
virtual-sink teardown, and tears down `PwContext` last: (1) guard `restore()`
and clear `pw_prev_default_sink`, (2) detach aux stream and call
`PwVirtualSource::stop()`, (3) `IVirtualSinkManager::teardown()` (stops both
captures, or unloads the pactl null-sinks on the fallback path),
(4) `PwContext::instance().shutdown()` — last, since every proxy/stream above
lives on its `pw_thread_loop`.

### Latency & realtime

Each pw_stream requests `node.latency = 256/48000` (~5.3 ms per graph hop)
and sets `PW_STREAM_FLAG_RT_PROCESS`; PipeWire's `module-rt` promotes the
graph thread to a realtime scheduling class when the session is configured
for it (standard on desktop distros with `pipewire.conf`'s default RT rules).

## Fallback matrix

| Platform | Backend | Notes |
|---|---|---|
| Linux, PipeWire daemon reachable | **Native `pw_stream`** (this doc) | Primary path — direct `Audio/Sink`/`Audio/Source` nodes, no PulseAudio round-trip. |
| Linux, daemon unreachable at startup | `pactl` null-sinks + `libpulse-simple` monitor capture | Legacy path (`LinuxPipewireSinks.cpp`), selected automatically by `VirtualSinkFactory` when `PwVirtualSinkManager::isAvailable()` is false. |
| Windows | None yet | Planned: WASAPI loopback capture, VB-CABLE interop. No virtual audio *endpoint* is possible without a signed kernel driver — CrossPad will not ship one; users install VB-CABLE and CrossPad will auto-detect it. |
| macOS | None yet | Planned: BlackHole auto-detection (same reasoning as Windows — no in-process virtual device without a signed driver/extension). |

## Build Requirements

`USE_PIPEWIRE` (default **ON**) gates the backend, on top of Linux +
`USE_AUDIO` + `USE_VIRTUAL_AUDIO`. CMake resolves headers/libs in order:

1. `pkg-config libpipewire-0.3` — normal path when `libpipewire-0.3-dev` is
   installed (`apt install libpipewire-0.3-dev` on Debian/Ubuntu).
2. No-root fallback: headers extracted to `~/.cache/crosspad/pw-dev/usr`
   (e.g. via `apt-get download` + `dpkg -x`, without installing), linked
   against the system's already-present `libpipewire-0.3.so.0`.
3. Neither found → `USE_PIPEWIRE` silently degrades to
   `CROSSPAD_PIPEWIRE_OK=FALSE` at configure time; the binary falls back to
   the `pactl` path at runtime (see fallback matrix above).

`RTAUDIO_API_PULSE` is still enabled on Linux so the legacy pactl fallback's
monitor sources remain discoverable by name.

## Testing

Native-backend tests live in `tests/audio/test_PwVirtualAudio.cpp`, tagged
`[pipewire]`. They call `PwContext::instance().init()` first and skip
gracefully (`SUCCEED(...)`) when no daemon is reachable (CI, containers) —
no daemon means no failure.

```bash
# Run only the PipeWire suite (skips cleanly without a daemon)
build/bin/crosspad_tests "[pipewire]"
```

One test, `PwDefaultSinkGuard takeover+restore roundtrip`, is tagged
`[pipewire][.mutate]` — the leading `.` hides it from default runs because it
mutates the *real* session default sink. It only executes when
`CROSSPAD_PW_TEST_MUTATE=1` is set in the environment:

```bash
CROSSPAD_PW_TEST_MUTATE=1 build/bin/crosspad_tests "[.mutate]"
```

## Troubleshooting (Linux)

| Symptom | Likely cause | Fix |
|---|---|---|
| `native PipeWire unavailable, falling back to pactl` in logs | No daemon reachable at startup, or built without PipeWire dev headers | `systemctl --user status pipewire`; rebuild with `libpipewire-0.3-dev` installed |
| `crosspad_vin*` sinks missing from `pavucontrol`/`qpwgraph` | Backend fell back to legacy pactl path, or `USE_VIRTUAL_AUDIO=OFF` | Check startup log for which backend was selected |
| System default sink didn't get restored | Killed with `SIGKILL` before shutdown ran | Next launch restores it automatically via `pw_prev_default_sink` crash recovery |
| Choppy audio | PipeWire quantum too low for your hardware | `pw-metadata -n settings 0 clock.force-quantum 512` |

## Roadmap

- Windows WASAPI loopback capture + VB-CABLE auto-detection
- macOS BlackHole auto-detection
- Settings-screen UI toggle for `pw_takeover_default` (currently prefs-file only)
- System tray with per-channel mute/volume (background operation)
