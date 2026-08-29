# PC sample engine

A streaming, polyphonic sample player for the simulator — the desktop
counterpart of Marcel's `ml_sample_stream_player` on the board, written against
the standard library only.

| File | What it is |
|---|---|
| `WavReader.{hpp,cpp}` | RIFF/WAVE parsing and framed reads. 16-bit PCM only; always emits interleaved stereo. |
| `SampleStreamEngine.{hpp,cpp}` | Slots, layers, voices, the streamer, the renderer. |
| `SampleStreamPlayer.{hpp,cpp}` | The board's `SampleStreamPlayer_*` API over that engine, so platform glue reads the same on both sides. |
| `PcSampleNode.{hpp,cpp}` | `crosspad::IAudioNode` generator for the audio module / mixer. |
| `PcSamplerPort.{hpp,cpp}` | Everything crosspad-sampler needs from this platform: the callback table, the kit manager, the kit-selector gate. |
| `test/` | The bench: material generator, scenarios, and the HW analyzers as judges. |

## How it streams

Samples are not loaded whole. Each layer keeps its first `kHeadFrames` (8192,
~170 ms at 48 kHz) resident and the rest arrives through a per-voice ring that
a background thread refills from that voice's own file handle. A hit therefore
sounds on the render call it arrived in, while the streamer has the length of
that head to open the file and get ahead of the playhead — a 200 MB kit costs
the same to trigger as a 200 kB one.

When the ring runs dry the voice **stalls** rather than skipping: a dropout is
audible, but a jump in the playhead is audible *and* leaves the streamer
chasing a position nothing asked for.

## Threads

`render` never blocks and never allocates. `stream` owns every `WavReader` and
every ring write. `control` (note on/off, setup, wipe) allocates and retires
voices. control and stream share one mutex; render takes none.

Two details are load-bearing, and each was a real defect first:

- **The producer acquires on the consumer's index.** `StreamRing::writable()`
  loads `r_` with acquire, not relaxed. Without it there is no happens-before
  between the consumer's last read and the producer's next write, and
  ThreadSanitizer reports the two `memcpy`s as a race on the first churn run.
- **A voice being freed waits out a render call in flight.** `releaseVoiceLocked()`
  publishes a non-Playing state and then spins on the voice's `rendering` flag;
  the renderer sets that flag *before* re-reading the state. Both sides are
  seq_cst, so exactly one sees the other. Bounded by one audio block, and the
  renderer never waits.

Voices hold a `shared_ptr` to their layer, so reloading a kit under a ringing
pad frees nothing the renderer is still reading — the board needs an explicit
render gate for the same situation.

## Wired in

`crosspad_app.cpp` brings the engine up through `sampler_port_init()`, which
also registers `PortableKitLoader` as the platform's `IKitManager`, installs
the sampler's callback table and makes it the system instrument. The node joins
`AudioMixerEngine` as a fourth channel ("Sampler", routed to OUT1) where the
mixer component is present, and the module's node chain otherwise. Mounting or
unmounting the virtual SD card calls `sampler_port_set_sdcard()`, which
re-points both the engine and the kit loader and rescans for kits.

The kit-selector gate is `sampler_port_pre_launch()` on the orchestrator: an
app that needs a kit and has none gets the browser first, exactly as on the
device.

Two things are worth knowing before concluding the audio path is dead:

- **At the launcher the mixer owns the pads.** `LoadMainScreen()` activates the
  mixer's pad logic, so a pad hit there toggles a mixer channel and never
  reaches a sample. The sampler takes the pads when its app opens, and keeps
  them afterwards because `sampler_service_init()` installs it as the *base*
  logic.
- **The saved mixer state can mute everything.** A `mixer_state.json` with the
  outputs muted, or with solo set on another channel, silences the sampler
  while every counter still says it is playing.
- **The sampler is not alone on the bus.** With the virtual-sink takeover on,
  everything the desktop plays arrives on IN1 and sums into OUT1 with the
  sampler — which is the point, but it means the two scenarios that measure
  quiet things (the silent tail, the bottom of the velocity ramp) cannot be
  taken. `run_sim_hil.py` samples the idle bus first and reports those as SKIP
  with the level it saw, rather than blaming the engine.

## Not portable, and deliberately so

The kit-selector icon (`notepick.png`) ships in the sampler app's own `assets/`
rather than being referenced from another repo's project images — that is what
gives it a tile everywhere instead of only on the board.

## Bench

`test/` runs the engine the way a crosspad-hil scenario runs a board: play
something real, capture what came out, count what the engine says happened, and
let the capture be judged separately.

```bash
python3 test/gen_wavs.py                       # test material, once
g++ -std=c++17 -O2 -pthread -I../../../lib/crosspad-core/include -I. \
    test/sampler_hil.cpp WavReader.cpp SampleStreamEngine.cpp \
    SampleStreamPlayer.cpp -o /tmp/sampler_hil
export CP_SD=<dir holding sd/crosspad/kits/TEST> CP_OUT=<capture dir>
/tmp/sampler_hil <scenario>
CP_OUT=$CP_OUT python3 test/judge.py           # analyzers, needs ~/GIT/crosspad-hil
```

Scenarios: `smoke`, `stream`, `sine_rt`, `onset`, `velocity`, `poly`, `choke`,
`loop`, `churn`, `storm`, `silence`, `edge`. Exit 0 = pass, 1 = the engine
failed, 2 = the bench is wrong.

`stream` is the one that proves streaming rather than measuring it: it renders
a 10 s file with the streamer pumped by hand and compares every frame against
the file. `churn` is the one that matters most, because it swaps every slot
while pads keep firing — a swap that only works from silence is not the thing
being tested, which is why it fails if no hit landed inside a swap window.

Run it under sanitizers too. ThreadSanitizer on `churn` is what found the ring
ordering bug above, and it needs `setarch -R` — it aborts with "unexpected
memory mapping" under ASLR on current kernels.

The sampler app's own workers (kit load, waveform loader) are FreeRTOS tasks on
every platform, the simulator included — see `SamplerRtos` in crosspad-sampler.
The engine's streamer is the one thread that is not: it belongs to the audio
engine, which is this platform's code, and it sits alongside RtAudio's own
threads rather than in the RTOS's schedule.

### Against the running simulator

`test/run_sim_hil.py` drives the whole stack — GUI, pad logic, event bus,
engine, mixer — through the simulator's own control port, and measures the
audio module's output level, which is the same shape as reading `SMPL_PEAK` off
a board over CDC.

```bash
./bin/CrossPad &                                   # exactly one instance
CP_SIM_LOG=<its stdout log> python3 test/run_sim_hil.py all
```

Scenarios: `gui`, `onset`, `velocity`, `silence`, `storm`, `churn`.

Four things about the bench, each of which cost an hour to learn:

- **Run exactly one simulator.** A second instance prints "Failed to bind port
  19840" and keeps running headless; the control port then belongs to whichever
  one started first, which may be a build from before your change. `pkill -x
  CrossPad` does not find them — the process `comm` is `Scheduler`, so match on
  the path instead.
- **One client at a time.** `handle_client()` runs inline in the accept loop, so
  the MCP daemon's persistent connection locks every other client out. Connect
  the script before anything else touches the port.
- **A press and a release in the same tick are invisible.** LVGL's indev polls
  at ~30 ms; `encoder_press` immediately followed by `encoder_release` never
  registers as a click, and neither does the `click` command on a list row,
  because it pushes SDL down and up together. Leave ~120 ms between them.
- **Load kits with `kit_load`, not by clicking.** The verb is the simulator's
  equivalent of the device's `KIT_LOAD`, and like it, the reply means the load
  *started* — `kit_status` is the honest answer. `[sampler] kit ready: … N
  pads, M layers` on stdout is what says the samples actually reached the
  engine; a kit that parses but loads nothing lights the pads exactly the same.
