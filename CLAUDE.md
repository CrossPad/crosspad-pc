# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Default Tools

**Prefer the CrossPad MCP tools** over raw shell for building, running, testing and driving the simulator. They live in the separate [crosspad-mcp](https://github.com/CrossPad/crosspad-mcp) repo (it used to sit here under `tools/mcp-server/`); its `crosspad` skill maps the tool set. The tools resolve this repo through `CROSSPAD_PC_ROOT`.

## Project Overview

CrossPad PC — a desktop simulator for the CrossPad embedded device. Runs the same LVGL GUI + crosspad-core/crosspad-gui libraries on desktop via SDL2 and FreeRTOS, with MIDI I/O (RtMidi), BLE MIDI (SimpleBLE), audio in/out (RtAudio; PipeWire virtual sinks on Linux), and an STM32 hardware emulator window (4x4 pad grid, rotary encoder, power button). Used for rapid development before deploying to the ESP32-S3 board (firmware: [platform-idf](https://github.com/CrossPad/platform-idf)).

## Build

Linux (Debian/Ubuntu):
```bash
sudo apt install build-essential cmake ninja-build pkg-config libsdl2-dev libasound2-dev libpulse-dev libdbus-1-dev
sudo apt install libpipewire-0.3-dev   # optional, native PipeWire backend
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```
Run: `bin/CrossPad` (or `scripts/run.sh`, which rebuilds when sources changed). `bin/CrossPad --versions` prints the core/gui/app commits it was built with.

Windows with MSVC (Visual Studio 2022 Community): `build.bat` calls `vcvarsall.bat x64`, wipes `build/`, then cmake+ninja with the vcpkg toolchain. Incremental:
```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```
Run: `bin/CrossPad.exe`. SDL2 via vcpkg (`vcpkg install sdl2:x64-windows`), vcpkg at `C:\vcpkg`.

macOS: the simulator runs, but CDC/MIDI/audio are not well tested — see `docs/building-macos.md`.

Tests: `ctest --test-dir build --output-on-failure -LE gui` (`-LE gui` skips the tests that need a display).

### CMake Options

FreeRTOS is always on (there is no `USE_FREERTOS` option any more).

| Option | Default | Description |
|---|---|---|
| `USE_MIDI` | ON | MIDI I/O via RtMidi (FetchContent) |
| `USE_AUDIO` | ON | Audio via RtAudio + FM synth (FetchContent) |
| `USE_BLE` | ON | BLE MIDI via SimpleBLE (FetchContent; `libdbus-1-dev` on Linux) |
| `USE_VIRTUAL_AUDIO` | ON | OS-visible virtual audio sinks (system mixer) |
| `USE_PIPEWIRE` | ON (Linux) | Native PipeWire virtual sinks/source; falls back to `pactl` at runtime when the daemon or dev headers are unavailable |
| `BUILD_TESTING` | ON | Catch2 tests (`crosspad_tests`, `gui_tests`) |
| `LV_USE_DRAW_SDL` | OFF | SDL GPU-accelerated drawing |
| `LV_USE_LIBPNG` | OFF | PNG decoding |
| `LV_USE_LIBJPEG_TURBO` | OFF | JPEG decoding |
| `LV_USE_FFMPEG` | OFF | Video playback |
| `LV_USE_FREETYPE` | OFF | FreeType font rendering |
| `ASAN` | OFF | AddressSanitizer (Debug, non-MSVC only) |

### Compile Defines

The `CrossPad` target gets: `PLATFORM_PC=1`, `USE_LVGL=1`, `USE_FREERTOS=1`, `CP_LCD_HOR_RES=320`, `CP_LCD_VER_RES=240`, plus `USE_MIDI=1`, `USE_AUDIO=1`, `USE_BLE=1`, `USE_VIRTUAL_AUDIO=1` when those options are ON.

### Core / GUI version gate

`src/crosspad_deps.hpp` declares the crosspad-core / crosspad-gui versions this simulator was written against (major mismatch is `#error`, minor/patch drift a `#warning`). Bump it together with the `lib/` submodules, after building and running against them. Keep the pins in step with what platform-idf's `main` uses.

## Architecture

### Entry Points & Init Flow

`src/freertos_main.cpp` is the only entry point: it handles `--versions`, starts the remote control server, creates the LVGL task (priority 1), which calls `crosspad_app_init()`, then starts the scheduler.

`crosspad_app_init()` in `src/crosspad_app.cpp` orchestrates the full init sequence:
1. `pc_platform_init()` — creates singletons (EventBus, Clock, PadManager, LedController, Settings, GuiPlatform)
2. `stm32Emu.init()` — builds emulator window, returns 320x240 LCD container
3. MIDI setup — auto-connect to "CrossPad" port, route NoteOn/Off → PadManager, CC → Encoder, SysEx → Stm32MessageHandler
4. Audio setup — RtAudio init, FM synth thread, VU meter timer
5. App registration — enumerate `AppRegistry`, create `App` wrappers
6. `initStyles()` + `LoadMainScreen()` — crosspad-gui theme and launcher

### Source Layout

```
src/
  freertos_main.cpp             — entry point (LVGL task + scheduler)
  crosspad_app.cpp              — shared init (MIDI, audio, apps, launcher)
  crosspad_deps.hpp             — required crosspad-core / crosspad-gui versions
  hal/hal.c                     — SDL2 HAL (display, mouse, keyboard, mousewheel)
  freertos/                     — FreeRTOS port glue (POSIX, Win32)
  stm32_emu/                    — device body: LCD, encoder, pads, jack panel, SD slot, keyboard capture
  midi/                         — PcMidi (RtMidi, auto-connect), PcBleMidi (SimpleBLE)
  audio/                        — RtAudio output/input, audio module, sampler port,
                                  pipewire/ (Linux virtual-audio backend, see docs/virtual-audio.md), virtual/
  synth/MlPianoSynth.cpp        — ISynthEngine impl wrapping ML_SynthTools FM engine
  sequencer/, song/             — PC ports of the pattern sequencer and core's song engine
  uart/, updater/               — PC stand-ins for the STM32 link and the updater
  remote/RemoteControl.cpp      — TCP server (localhost:19840) for MCP integration
  pc_stubs/
    PcPlatformStubs.cpp         — PC impls: PcClock, PcLedStrip, PcKeyValueStore, PcGuiPlatform, etc.
    PcApp.cpp                   — lightweight App class for launcher (no sequencer/CLI)
    PcHttpClient.cpp            — HTTP for the App Store / updater
    pc_platform.h               — public API: pc_platform_init(), set_midi/audio/synth
  apps/settings, citest, update — built-in apps
  apps/crosspad-*/              — installed apps (submodules)
lib/ml_synth/                   — vendored ML_SynthTools FM synth engine
lib/crosspad-core/              — submodule: portable C++ library
lib/crosspad-gui/               — submodule: shared LVGL UI components
scripts/
  app_manager.py                — Python app manager (wrapper for crosspad-apps core)
  run.sh                        — Smart build+run script
tests/                          — Catch2 tests (ctest labels: gui, flaky)
```

### Submodules

**Shared libraries** (in `lib/`):

- **crosspad-core**: Portable C++ library — AppRegistry, IEventBus, PadManager, PadLedController, CrosspadSettings (IKeyValueStore), Stm32MessageHandler, platform interfaces (IClock, IMidiOutput, ILedStrip, IAudioOutput, ISynthEngine). Originally an ESP-IDF component; sources are auto-discovered (`file(GLOB_RECURSE)`) in CMakeLists.txt.
- **crosspad-gui**: LVGL UI components — theme, styles, launcher, status bar, widgets (keypad buttons, spinbox, radial menu, VU meter, file explorer, DFU panel, modals/toasts). Originally an ESP-IDF component; `.cpp` and `.c` sources (fonts, icons) auto-discovered.
- **lvgl**: LVGL v9.x graphics library
- **FreeRTOS**: FreeRTOS Kernel (MSVC-MingW port on Windows, GCC POSIX on Linux/Mac)

**Installable apps** (in `src/apps/crosspad-*/`, managed by app manager):

- **crosspad-appstore**: Built-in App Store (cannot be removed)
- **crosspad-mixer**, **crosspad-piano**, **crosspad-instructions**, **crosspad-serial-monitor**: Installable via `python3 scripts/app_manager.py install <name>` or via the App Store UI
- **crosspad-sequencer**, **crosspad-song**, **crosspad-arrange**, **crosspad-pad-mixer**, **crosspad-dawcontrol**: the board's apps, with the platform services they need wired here -- the pattern sequencer and metronome (`src/sequencer/PcSequencer.cpp`, scenes in `<sdcard_path>/crosspad/sequences.json`), core's song engine (`src/song/PcSongEnginePort.cpp`, projects in `<sdcard_path>/SONGS`, bounces from the mixer's third "Rec (pads)" bus), per-pad levels (`PcPadMix` in `PcSamplerPort.cpp`) and `stm32MessageHandler` for DAW Control (no hub on PC, so no launch buttons)

### App Management

Apps are installed as git submodules in `src/apps/crosspad-*/`. CMake auto-discovers them via `file(GLOB)`. The app manager (`scripts/app_manager.py`) wraps the shared [crosspad-apps](https://github.com/CrossPad/crosspad-apps) core.

**CLI commands:**

```bash
python3 scripts/app_manager.py list              # List available apps
python3 scripts/app_manager.py install mixer     # Install an app
python3 scripts/app_manager.py remove mixer      # Remove an app
python3 scripts/app_manager.py update --all      # Update all installed apps
python3 scripts/app_manager.py sync              # Sync manifest with disk
python3 scripts/app_manager.py                   # Launch interactive TUI
```

**After install/remove:** `cmake -B build -G Ninja && cmake --build build` (CMake reconfigure picks up new sources).

**Creating a new app:** See the [crosspad-appstore README](https://github.com/CrossPad/crosspad-appstore#creating-a-crosspad-app) for the app repo structure, `crosspad-app.json` format, and registration pattern.

### Platform Abstraction Pattern

crosspad-core defines portable interfaces; this repo provides PC implementations:

| Interface | PC Implementation | Notes |
|---|---|---|
| `IClock` | `PcClock` (std::chrono) | |
| `IEventBus` | core's `FreeRtosEventBus` | `post*()` queued to a dispatch task, `send*()` synchronous |
| `ILedStrip` | `PcLedStrip` | 16 virtual RGB pixels, readable via `pc_get_led_color()` |
| `IMidiOutput` | `PcMidi` / `NullMidiOutput` | Swappable at runtime via `pc_platform_set_midi_output()` |
| `IAudioOutput` | `PcAudioOutput` / `NullAudioOutput` | Swappable via `pc_platform_set_audio_output()` |
| `ISynthEngine` | `MlPianoSynth` | FM synth, swappable via `pc_platform_set_synth_engine()` |
| `IKeyValueStore` | `PcKeyValueStore` | Filesystem-backed persistence |
| `IGuiPlatform` | `PcGuiPlatform` | Display dimensions, update notifications |

Singletons accessed via `crosspad::getPadManager()`, `crosspad::getEventBus()`, etc. — initialized in `pc_platform_init()`.

### Platform Capabilities

crosspad-core provides a bitflag-based capability query system (`crosspad/platform/PlatformCapabilities.hpp`). Platforms declare what they support; apps query at runtime instead of null-checking interface pointers.

**Available flags** (`enum class Capability : uint32_t`): `Midi`, `AudioOut`, `AudioIn`, `Synth`, `Pads`, `Leds`, `Encoder`, `Display`, `Persistence`, `Vibration`, `WiFi`, `Bluetooth`, `Usb`, `Imu`, `Stm32`, `Sequencer`.

**API:**
- `setPlatformCapabilities(caps)` — set all flags at once (call during init)
- `addPlatformCapability(cap)` / `removePlatformCapability(cap)` — modify at runtime
- `hasCapability(cap)` — true if ALL specified flags are present
- `hasAnyCapability(caps)` — true if ANY specified flag is present

**PC sets:** base caps (Pads, Leds, Encoder, Display, Persistence) in `pc_platform_init()`, then adds Midi/AudioOut/AudioIn/Synth as devices connect in `crosspad_app_init()`.

**ESP32-S3 sets:** full caps (all hardware) in `ArduinoCrosspadPlatform_Init()`, adds Vibration when driver registers.

### App System

Apps are registered via crosspad-core's `AppRegistry` using static `AppRegistrar` constructors. The PC `App` class (`src/pc_stubs/PcApp.hpp`) is a lightweight wrapper — no sequencer, CLI, or kit loader — supporting lifecycle (`start`/`pause`/`resume`/`destroyApp`) and launcher integration. Apps receive pad/MIDI events through the EventBus → AppManagerBase → IApp callback chain.

**Adding a new app:** Create a registration function (like `_register_MLPiano_app()`) that calls `AppRegistrar` with `createLVGL`/`destroyLVGL` function pointers, then call it from `crosspad_app_init()`.

### Event & Input Flow

```
Pad click (EmuPadGrid) or MIDI NoteOn → PadManager::handlePadPress()
  ├→ PadLedController (LED animation)
  ├→ IEventBus::postPadPressed() → subscribed apps' onPadPressed()
  └→ IPadLogicHandler::onPadPressed() (if active logic set)

MIDI CC1 → Stm32EmuWindow::handleEncoderCC() → encoder rotation
MIDI CC64 → encoder button press
MIDI SysEx → Stm32MessageHandler → PadManager LED updates
```

### Audio Pipeline

```
App noteOn/noteOff → MlPianoSynth (ISynthEngine)
  → Audio thread: fmSynth.process() → pcAudio.write() (ring buffer)
  → RtAudio callback: ring buffer → WASAPI → speakers
  → VU meter timer: pcAudio.getOutputLevel() → crosspad_gui::vu_set_levels()
```

## Compatibility with ESP32-S3

This simulator must stay compatible with the ESP32-S3 firmware ([platform-idf](https://github.com/CrossPad/platform-idf)). Key constraints:

- **App interface:** All apps implement crosspad-core's `IApp` interface (business-logic callbacks: `onNoteOn`, `onPadPressed`, etc.). The PC `App` class can be simpler than ESP32's (no sequencer/CLI), but must support the same lifecycle and AppRegistry pattern.
- **PadManager is the single source of truth** for pad state, note mapping, and LED coordination. Always route pad events through PadManager, not directly to apps.
- **Settings persistence** uses `IKeyValueStore` abstraction — ESP32 uses NVS, PC uses filesystem. `CrosspadSettings` singleton must be loaded/saved through this interface.
- **crosspad-gui components** are shared — launcher, status bar, styles. The PC simulator sets `IGuiPlatform` for display dimensions and hooks.
- **LCD resolution** is 320x240 (`CP_LCD_HOR_RES`/`CP_LCD_VER_RES`), matching hardware. The SDL window is larger (490x660) to include the emulator body.

### Remote Control Server

The simulator includes a built-in TCP server (`src/remote/RemoteControl.cpp`) on `localhost:19840` for external automation. Started automatically with the simulator. Protocol: newline-delimited JSON.

Commands include `ping`, `screenshot`, `click`, `pad_press`/`pad_release`, `power_click`/`power_hold`/`power_press`/`power_release`, `encoder_rotate`/`encoder_press`/`encoder_release`, `key`, `stats`, `settings_get`/`settings_set`, `midi_note_on`/`midi_note_off`, `kit_list`/`kit_load`/`kit_status`, audio device and level commands — the dispatch in `RemoteControl.cpp` is the full list.

Used by the [crosspad-mcp](https://github.com/CrossPad/crosspad-mcp) simulator tools.

## MCP Development Server

Moved out of this repo to [crosspad-mcp](https://github.com/CrossPad/crosspad-mcp) (install and tool reference there). It talks to a running simulator over the TCP control port above; build, repo and code-search tools work without one.

## Important Notes

- crosspad-core and crosspad-gui use `idf_component_register()` in their own CMakeLists.txt (ESP-IDF build system). This project bypasses that by globbing their sources in the top-level CMakeLists.txt.
- Third-party code (LVGL, RtMidi, RtAudio, ml_synth) is compiled with warnings suppressed (`/w` on MSVC). Project code uses default warning levels.
- `LV_USE_FS_WIN32` is enabled with driver letter `'C'` for Windows file system access.
- FreeRTOS heap is 32 MB (`config/FreeRTOSConfig.h`). The MSVC-MingW port uses Windows threads under the hood.
- FetchContent pulls ArduinoJson 7.3.0 (required by crosspad-core settings), RtMidi 6.0.0, RtAudio 6.0.0, SimpleBLE and Catch2.

## CrossPad Manifesto

### Write once, run everywhere
CrossPad runs the same core logic on ESP32-S3 hardware and desktop simulators. Platform differences are hidden behind thin abstraction layers — not thick frameworks. Business logic should never know what chip it's running on.

### Platform repos are thin, shared repos are thick
Cross-platform is a first-class goal. Platform-specific repositories (ESP32-S3, 2playerCrosspad, crosspad-pc) should contain only what cannot be shared — hardware drivers, HAL bindings, build system glue. All business logic belongs in crosspad-core; all UI components belong in crosspad-gui. If you're writing code that could work on another platform, it doesn't belong in a platform repo.

### Hardware is software you can touch
We design hardware and software as one system. The pad grid, the encoder, the LED strip — they are first-class citizens with well-defined interfaces. If you can simulate it on a PC, you can ship it on a board.

### Small team, big surface area
We are a small team. This is a feature, not a limitation. Every architectural decision is made to reduce maintenance burden: shared init sequences, unified event buses, portable abstractions. Code that exists in two places will eventually exist in one.

### Open by default
Schematics, firmware, PC tools, documentation — all open source. Not because it's trendy, but because a music controller you can't modify isn't yours. We want people to fork, adapt, and build things we never imagined.

### Community-driven, not committee-driven
We welcome contributions, but we ship fast. A clear architecture and good documentation lower the barrier to entry. You shouldn't need to read the entire codebase to add an app or write a new platform driver.

### Documentation is not an afterthought
If it's not documented, it doesn't exist. API references, architecture guides, build instructions — they ship with the code, not after it.
