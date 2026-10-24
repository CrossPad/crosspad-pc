# PC sampler

The simulator plays sampler kits with the engine the board plays them with:
crosspad-core's `crosspad::SamplerEngine` (`crosspad/sampler/*`). What is here
is the platform's side of it — where bytes and memory come from, which thread
streams — and the crosspad-sampler app's callback table over it. The engine's
behaviour (voices, residency, the stream window, realtime pitch, choke groups,
Loop pads that launch on the bar) is core's, so it is the same on both, and it
is tested there (`tests/test_SamplerEngine.cpp`).

| File | What it is |
|---|---|
| `PcSamplerEngine.{hpp,cpp}` | The engine's platform side: the source (stdio over the virtual SD card), the heap, the streaming thread, the kit take and the pad swap, the "Sampler" mixer channel. The board's counterpart is platform-idf's `components/crosspad-platform-idf/SamplerEnginePort.cpp`. |
| `PcSamplerPort.{hpp,cpp}` | Everything crosspad-sampler needs from this platform: the callback table, the kit manager, the kit-selector gate, the SamplePlayback bridge. The board's is in platform-idf's `main/gui/gui.cpp`. |
| `PcPitchedPort.{hpp,cpp}` | A kit with `"engine": "pitched"`: core's pitched instrument on its own channel. |
| `PcInstrumentHost.{hpp,cpp}` | An app borrowing the pitched engine (an Arrange Pitch track, a song's instrument). |
| `test/run_sim_hil.py` | Scenarios against the running simulator, over its control port. |
| `test/make_loop_kit.py` | The fixture the `loop` scenario plays. |

## Where it differs from the board

Only in what the board has to ration:

- **Memory** is the heap. The board carves heads and stream blocks out of an
  arena it shares with the pitched engine and the song rings; here the two
  engines can hold their kits at once, so nothing is handed over between them.
  The block sizes, the ring depth and the 256 kB head a sample asks for are
  the board's, so a kit streams here the way it does on the card; only the
  budget the heads are halved to fit is larger.
- **The kit is read on the thread that asks** — the kit loader's worker for a
  kit load, the pad editor for a pad edit — under one loader lock. The board
  has a take task and a pad task because its LVGL and CDC stacks are small.
- **The streaming pass** is a `std::thread` beside RtAudio's, once a
  millisecond — the board's audio_bg. It also hands the engine's voice reports
  on to the LEDs and the Sampler panel, which the render must not touch.
- **The audition** (the file browser) plays on the "Sampler" channel; the board
  gives it a "Preview" channel of its own. It plays with no kit loaded too.

## How a kit gets in

`reloadPad()` per pad, from the kit loader. The first pad of a kit the engine
does not hold takes the whole kit: every sample's header is read, HeadPlan
sizes the heads, every pad is built. Pads of another kit after that are
swapped one at a time under whatever is playing: the pad's new samples are
read in beside the old ones and the pad is pointed at them in one step; a
voice already sounding plays its old sample out, and a sample no pad names
goes back once no voice reads it (`SamplerEngine::slotInUse()`, swept by the
streaming thread). A pitched kit releases the sample engine on its way in.

A pad whose filters or reverse call for a rendered copy (`renderedSamplePath`)
plays that copy; pitch alone the engine does in the voice
(`engineHandlesFx()`).

## Loop pads (platform-idf#82)

A pad with `playbackMode` 2 is a launch/mute toggle, in core:

- transport stopped: a hit starts it at once, the next stops it;
- transport running: a hit launches it on the next bar line, to the frame; the
  next mutes it (the voice keeps advancing, so the unmute is in phase), the
  next unmutes; a hit on a launch still waiting cancels it;
- the transport stopping ends every loop. A release is ignored.

The bar clock is the metronome (`MetronomeNode::framesToBar()`), a mixer
channel added after the sampler, so in the render it still stands where the
sampler's block starts. A loop waiting for its bar or muted shows its playing
colour dimmed (`PadLedController::notifyNoteDimmed()`). The emulator's pad grid
paints the pads' own colours, not what the controller painted, so the dim is
read from the control port's `led_state` `strip`, not from a screenshot.

`smpl_loops` on the control port is the board's `SMPL_LOOPS` (each pad `.`
idle, `q` waiting, `P` playing, `m` muted; the launch counters; the clock),
`seq {action: play|stop|state, bpm, countin}` the board's `SEQ PLAY/STOP/BPM`.

## Wired in

`crosspad_app.cpp` brings the engine up through `sampler_port_init()`, which
also registers `PortableKitLoader` as the platform's `IKitManager`, installs
the sampler's callback table and makes it the system instrument. The node
joins `AudioMixerEngine` as a fourth channel ("Sampler", routed to OUT1) where
the mixer component is present, and the module's node chain otherwise.
Mounting or unmounting the virtual SD card calls `sampler_port_set_sdcard()`,
which re-points the engine's paths and the kit loader and rescans for kits.

A finger's hit goes straight into the engine (`playPad`, `armPad` before its
velocity); notes from the sequencer and MIDI arrive as `SamplePlayback`
events.

Two things are worth knowing before concluding the audio path is dead:

- **At the launcher the mixer owns the pads.** `LoadMainScreen()` activates the
  mixer's pad logic, so a pad hit there toggles a mixer channel and never
  reaches a sample. The sampler takes the pads when its app opens, and keeps
  them afterwards because `sampler_service_init()` installs it as the *base*
  logic.
- **The saved mixer state can mute everything.** A `mixer_state.json` with the
  outputs muted, or with solo set on another channel, silences the sampler
  while every counter still says it is playing.

## Against the running simulator

```bash
python3 test/make_loop_kit.py <sdcard>/crosspad/kits     # once, for `loop`
CROSSPAD_REMOTE_PORT=19841 CROSSPAD_PROFILE_DIR=<scratch> CROSSPAD_NO_MIDI=1 \
    CROSSPAD_AUDIO=off ./bin/CrossPad &
CROSSPAD_REMOTE_PORT=19841 CP_SIM_LOG=<its stdout> python3 test/run_sim_hil.py all
```

Scenarios: `gui`, `onset`, `velocity`, `silence`, `storm`, `churn`, `loop`,
and the control-port checks `audio`, `mixer`, `pitched`, `wave`, `apps`,
`smpl`, `led`. `onset`, `storm` and `churn` want kits with a sample on every
pad; `loop` the LOOPTEST kit.

`CROSSPAD_AUDIO=off` still renders — the audio module paces itself with no
device — so every level the scenarios read is real. `CROSSPAD_NO_MIDI` keeps
the simulator off a real CrossPad on USB: without it the MIDI auto-connect
finds the board's port.

Four things about the bench, each of which cost an hour to learn:

- **One simulator per port.** A second instance on a port that is taken prints
  "Failed to bind port" and keeps running headless. `pkill -x CrossPad` does
  not find it — the process `comm` is `Scheduler`, so match on the path.
- **One client at a time.** `handle_client()` runs inline in the accept loop,
  so the MCP daemon's persistent connection locks every other client out.
- **A press and a release in the same tick are invisible** to LVGL's indev
  (~30 ms poll); leave ~120 ms between them.
- **Load kits with `kit_load`, not by clicking.** Like the device's
  `KIT_LOAD`, the reply means the load *started* — `kit_status` is the honest
  answer, and `[sampler] kit ready: … N pads, M layers` on stdout says the
  samples reached the engine.

## A song's second kit

`PcSecondKit` implements core's `ISecondKit` for a song's second kit track: a
`SamplerEngine` of its own on the mixer channel "Kit 2" (OUT1 and the capture
bus), published as `PlatformServices::secondKit`. A worker reads the kit named
by `hold()` with the same readers and pad builders as the pads' engine
(`PcKitLoad`), a streaming thread of its own services its rings, and a note
reaches it by `noteOn()`/`noteOff()` only. The kit picker is the sampler's kit
gate (`PlatformServices::kitPicker`), installed with the pads' engine.

A release never frees what the audio thread may be reading: the node counts the
render passes and notes inside the engine, a release closes the gate and waits
for the count to reach zero, then gives the samples back (the board waits a
fixed 10 ms instead). `tests/audio/test_SecondKit.cpp` holds and releases kits
under a running render thread.
