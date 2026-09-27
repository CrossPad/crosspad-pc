# Audio Plugin Hosting — Roadmap

This is a roadmap, not a shipped feature. It records the tiered plan for
letting third-party (and first-party) DSP process CrossPad's audio, from
"already possible today" to "long-term in-process host."

The guiding constraint is the CrossPad manifesto's *write once, run
everywhere*: whatever we build here should not lock CrossPad's DSP into a
PC-only plugin ABI. The portable surface is `crosspad::IAudioNode` — the
same node type the mixer engine (`AudioMixerEngine`) runs on PC **and** on
ESP32-S3. Every tier below either builds on that interface directly or keeps
it as the escape hatch for anything that must also run on hardware.

## Available today: monitor-port taps

The native PipeWire virtual sinks (`crosspad_vin1`/`crosspad_vin2`, see
[docs/virtual-audio.md](virtual-audio.md)) are `Audio/Sink` nodes, so
PipeWire gives them monitor ports for free. External tools can already patch
into CrossPad's input signal without any code changes here — e.g. open
`qpwgraph`, link a monitor port to a LADSPA/LV2 host node, and route the
processed output back into `crosspad_vin1`. This is a manual, external-graph
workflow — the tiers below are about making equivalent (and richer)
processing a first-class, in-app feature.

## Tier 1 — Custom DSP as `crosspad::IAudioNode`

**Status: the existing, portable answer.** Any effect written against
`IAudioNode` and added to `AudioMixerEngine` (`s_mixerEngine.addChannel(...)`
/ insert-style processing) runs identically on the PC simulator and on
ESP32-S3 firmware — no `#ifdef PLATFORM_PC` branches, no separate DSP code
path. This is the only tier where "write once" is absolute: a filter, a
compressor, a custom pad-triggered effect written here ships to hardware
unchanged.

Use this tier whenever the DSP is CrossPad's own — anything the product
needs by default, or anything that must survive on the embedded target.

## Tier 2 — `module-filter-chain` for LV2/LADSPA inserts

PipeWire ships `libpipewire-module-filter-chain`, which hosts LV2 or LADSPA
plugins as an ordinary PipeWire filter node — no plugin-hosting code to
write. CrossPad can load one with:

```cpp
pw_context_load_module(context, "libpipewire-module-filter-chain",
                        /* args: JSON describing the LV2/LADSPA plugin(s)
                           to chain, node name, format */, nullptr);
```

The loaded filter node behaves like any other PipeWire node — it can be
patched between a `crosspad_vin*` sink and the mixer input (or after OUT1,
ahead of `crosspad_out`) the same way `qpwgraph` patches things today, except
CrossPad owns the connection instead of requiring the user to do it by hand.

This is Linux-only (PipeWire-specific) and does not run on ESP32-S3 — it is
strictly a PC-side convenience for hosting community LV2/LADSPA effects
without writing a plugin host. Not yet implemented; the module-load call
above is the shape of the eventual integration point.

## Tier 3 — Insert patch-points + external host (VST2/VST3)

For the VST ecosystem, CrossPad will expose named insert patch-points (PipeWire
nodes analogous to Tier 2's filter-chain output) that a full plugin host can
connect to. The intended host is **Carla**, reachable over PipeWire via
`pipewire-jack` (Carla's JACK client transparently rides PipeWire's JACK
compatibility layer) — no JACK server needed separately.

VST3 works this way out of the box (open SDK). **VST2 does not get first-class
in-tree support**: Steinberg's VST2 SDK was pulled and its license is
effectively dead upstream, so VST2 hosting is only ever available *through*
an external host like Carla that already carries its own (older, grandfathered)
VST2 support — CrossPad will never vendor a VST2 SDK or write a VST2 host
itself.

Not yet implemented. This tier is PC-only by nature (VST hosting has no
embedded equivalent) and sits entirely outside the `IAudioNode` portability
contract — which is exactly why it's kept at arm's length via patch-points
rather than baked into the mixer engine.

## Tier 4 — Embedded CLAP host (long-term)

The long-term in-process answer is a small embedded **CLAP** host. CLAP
(CLever Audio Plugin) is MIT-licensed with a plain C API, which makes it
realistic to host in-process (unlike VST2/VST3's C++ ABI and licensing
constraints) and — unlike Tiers 2/3 — plausible to eventually constrain to a
subset that could run in an embedded-friendly host, keeping the door open to
partial hardware portability that Tiers 2/3 categorically don't have.

Not yet implemented. This is a multi-week undertaking (plugin scanning,
parameter automation, GUI embedding or headless parameter control) and is
tracked here as direction, not a committed date.

## Summary

| Tier | Mechanism | Portable to ESP32-S3? | Status |
|---|---|---|---|
| 0 | Monitor-port tap via external graph tool (`qpwgraph`) | No (PC-only, manual) | Available today |
| 1 | `crosspad::IAudioNode` in `AudioMixerEngine` | **Yes** | Existing, in use |
| 2 | `module-filter-chain` (LV2/LADSPA) | No | Planned |
| 3 | Insert patch-points + Carla via `pipewire-jack` (VST2/VST3) | No | Planned |
| 4 | Embedded CLAP host | No (partial portability aspiration) | Long-term |
