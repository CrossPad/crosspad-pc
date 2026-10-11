# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Default Tools

**Prefer the CrossPad MCP tools** over raw shell for building, running, testing and driving the simulator. They live in the separate [crosspad-mcp](https://github.com/CrossPad/crosspad-mcp) repo (it used to sit here under `tools/mcp-server/`); its `crosspad` skill maps the tool set, and https://crosspad.app/docs/reference/mcp-tools/ lists every tool. The tools resolve this repo through `CROSSPAD_PC_ROOT`. Build, repo and code-search tools work without a running simulator; the `[PC sim]` tools talk to it over the TCP control port (§ Remote Control Server).

## Project Overview

CrossPad PC — a desktop simulator for the CrossPad embedded device. Runs the same LVGL GUI + crosspad-core/crosspad-gui libraries on desktop via SDL2 and FreeRTOS, with MIDI I/O (RtMidi), BLE MIDI (SimpleBLE), audio in/out (RtAudio; PipeWire virtual sinks on Linux, see `docs/virtual-audio.md`), and an STM32 hardware emulator window (4x4 pad grid, rotary encoder, power button). Used for rapid development before deploying to the ESP32-S3 board (firmware: [platform-idf](https://github.com/CrossPad/platform-idf)). What differs from the board: [Platform parity](https://crosspad.app/docs/concepts/platform-parity/).

## Docs

Setup, build, run and tests for Linux, macOS and Windows are on the site, not here:

- https://crosspad.app/docs/start-here/pc-simulator/ — deps, clone, build, run, tests
- https://crosspad.app/docs/troubleshooting/#simulator — SDL2/ALSA/DBus not found, `build.bat` without vcvarsall, no window, port 19840 taken, vcpkg
- https://crosspad.app/docs/how-to/new-app/ — writing an app (the "No board? Use the PC simulator" aside covers `scripts/app_manager.py new`)
- https://crosspad.app/docs/concepts/architecture/, https://crosspad.app/docs/concepts/where-code-belongs/, https://crosspad.app/docs/concepts/manifesto/

A change that breaks a docs page (command, output, path) opens an issue in CrossPad/crosspad-docs.

## Build

Day to day: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build`, run `bin/CrossPad` (`bin\CrossPad.exe`), test with `ctest --test-dir build --output-on-failure -LE gui`. Gotchas the site does not need:

- `scripts/run.sh` rebuilds (Linux/macOS, Ninja, Debug) when any source under `src/`, `lib/crosspad-core/src` or `lib/crosspad-gui/src` is newer than `bin/CrossPad`, then launches it. CMake also has a `run` target.
- `bin/CrossPad --versions` prints one `APPVER: <component> id=… commit=… ref=… dirty=…` line per core / gui / app it was built with, then `APPVER: end count=N`, and exits — the simulator's answer to the firmware's APP_VERSIONS, used by the app manager.
- Windows: `build.bat` calls Visual Studio 2022 **Community**'s `vcvarsall.bat x64`, **wipes `build/` every time**, then configures with the vcpkg toolchain (`C:/vcpkg/scripts/buildsystems/vcpkg.cmake`). For incremental builds run the two cmake lines by hand from an x64 Native Tools prompt. A post-build step copies `SDL2.dll` (from the vcpkg tree) next to the exe.
- Linux: `libdbus-1-dev` is only needed for `USE_BLE` (SimpleBLE); without it configure with `-DUSE_BLE=OFF`. `libpipewire-0.3-dev` is optional: without it CMake prints "PipeWire dev headers not found" and the virtual sinks fall back to `pactl` at runtime.
- macOS: the simulator runs, but CDC/MIDI/audio are not well tested. CMakeLists.txt has no `APPLE` branch; `docs/building-macos.md` lists the open items.
- `ctest -LE gui` skips the tests that need a display (`gui_integration`, and `gui_audio_citest`, which is also labelled `flaky`); `ctest -L gui -LE flaky` runs the stable GUI one.

### CMake Options

FreeRTOS is always on (there is no `USE_FREERTOS` option any more).

| Option | Default | Description |
|---|---|---|
| `USE_MIDI` | ON | MIDI I/O via RtMidi (FetchContent) |
| `USE_AUDIO` | ON | Audio via RtAudio + FM synth (FetchContent) |
| `USE_BLE` | ON | BLE MIDI via SimpleBLE (FetchContent; `libdbus-1-dev` on Linux) |
| `USE_VIRTUAL_AUDIO` | ON | OS-visible virtual audio sinks (system mixer) |
| `USE_PIPEWIRE` | ON (Linux) | Native PipeWire virtual sinks/source; falls back to `pactl` at runtime when the daemon or dev headers are unavailable |
| `BUILD_TESTING` | ON | Catch2 tests (`crosspad_tests`, `gui_tests`). OFF = release build: the App Store app and `CROSSPAD_DEV_BUILD` are left out |
| `LV_USE_DRAW_SDL` | OFF | SDL GPU-accelerated drawing |
| `LV_USE_LIBPNG` | OFF | PNG decoding |
| `LV_USE_LIBJPEG_TURBO` | OFF | JPEG decoding |
| `LV_USE_FFMPEG` | OFF | Video playback |
| `LV_USE_FREETYPE` | OFF | FreeType font rendering |
| `ASAN` | OFF | AddressSanitizer (Debug, non-MSVC only; a plain cache variable, not an `option()`) |

### Compile Defines

The `CrossPad` target gets: `PLATFORM_PC=1`, `USE_LVGL=1`, `USE_FREERTOS=1`, `CP_LCD_HOR_RES=320`, `CP_LCD_VER_RES=240`, plus `USE_MIDI=1`, `USE_AUDIO=1`, `USE_BLE=1`, `USE_VIRTUAL_AUDIO=1` when those options are ON, and `CROSSPAD_DEV_BUILD=1` when `BUILD_TESTING` is ON.

### Core / GUI version gate

`src/crosspad_deps.hpp` declares the crosspad-core / crosspad-gui versions this simulator was written against (today core 1.53.0, gui 1.23.1; major mismatch is `#error`, minor/patch drift a `#warning`). Bump it together with the `lib/` submodules, after building and running against them. Keep the pins in step with what platform-idf's `main` uses.

## Architecture

### Entry Points & Init Flow

`src/freertos_main.cpp` is the only entry point: `main()` handles `--versions`, creates the LVGL task (priority 1) and starts the scheduler. The LVGL task opens the SDL window, calls `crosspad_app_init()`, then starts the remote control server (`remote::start`) and runs the LVGL loop.

`crosspad_app_init()` in `src/crosspad_app.cpp` orchestrates the full init sequence:
1. `pc_platform_init()` — creates singletons (EventBus, Clock, PadManager, LedController, Settings, GuiPlatform)
2. `stm32Emu.init()` — builds emulator window, returns 320x240 LCD container
3. MIDI setup — auto-connect to the saved ports, else the first whose name contains "CrossPad" (port 0 if none), route NoteOn/Off → PadManager, CC → Encoder, SysEx → Stm32MessageHandler
4. Audio setup — RtAudio devices, FM synth, mixer engine, `PcAudioModule` thread, VU meter timer
5. App registration — `AppRegistry_InitAll()` (generated at configure time, § App System), then the orchestrator builds the launcher list
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
    pc_platform.h               — public API: pc_platform_init(), OUT2/input setters, LED readback, PC-only settings (update check, USB autoconnect, SD path)
  apps/settings, citest, update — built-in apps
  apps/crosspad-*/              — installed apps (submodules)
lib/ml_synth/                   — vendored ML_SynthTools FM synth engine
lib/crosspad-core/              — submodule: portable C++ library
lib/crosspad-gui/               — submodule: shared LVGL UI components
lvgl/, FreeRTOS/                — upstream submodules
cmake/generate_registry.cmake   — generates app_registry_init.cpp (AppRegistry_InitAll)
scripts/
  app_manager.py                — Python app manager (wrapper for crosspad-apps core)
  run.sh                        — Smart build+run script
tests/                          — Catch2 tests (ctest labels: gui, flaky)
docs/                           — design notes; instructions.md is the in-app Help text
```

### Submodules

**Shared libraries** (in `lib/`):

- **crosspad-core**: Portable C++ library — AppRegistry, IEventBus, PadManager, PadLedController, CrosspadSettings (IKeyValueStore), Stm32MessageHandler, platform interfaces (IClock, IMidiOutput, ILedStrip, IAudioOutput, ISynthEngine). Originally an ESP-IDF component; sources are auto-discovered (`file(GLOB_RECURSE)`) in CMakeLists.txt, minus `EspEventBus`, `SyncEventBus` and `Stm32Manager`.
- **crosspad-gui**: LVGL UI components — theme, styles, launcher, status bar, widgets (keypad buttons, spinbox, radial menu, VU meter, file explorer, DFU panel, modals/toasts). Originally an ESP-IDF component; `.cpp` and `.c` sources (fonts, icons) auto-discovered.
- **lvgl**: LVGL v9.x graphics library
- **FreeRTOS**: FreeRTOS Kernel (MSVC-MingW port on Windows, GCC POSIX on Linux/Mac)

**Installable apps** (in `src/apps/crosspad-*/`, managed by app manager):

- **crosspad-appstore**: the App Store (PC only). It hides itself from its own list, so it cannot remove itself, and release builds (`BUILD_TESTING=OFF`) leave it out
- **crosspad-mixer**, **crosspad-piano**, **crosspad-instructions**, **crosspad-serial-monitor**, **crosspad-sampler**: Installable via `python3 scripts/app_manager.py install <name>` or via the App Store UI
- **crosspad-sequencer**, **crosspad-song**, **crosspad-arrange**, **crosspad-pad-mixer**, **crosspad-dawcontrol**: the board's apps, with the platform services they need wired here -- the pattern sequencer and metronome (`src/sequencer/PcSequencer.cpp`, scenes in `<sdcard_path>/crosspad/sequences.json`), core's song engine (`src/song/PcSongEnginePort.cpp`, projects in `<sdcard_path>/SONGS`, bounces from the mixer's third "Rec (pads)" bus), per-pad levels (`PcPadMix` in `PcSamplerPort.cpp`) and `stm32MessageHandler` for DAW Control (no hub on PC, so no launch buttons)

### App Management

Apps are installed as git submodules in `src/apps/crosspad-*/`. CMake auto-discovers them via `file(GLOB)`. The app manager (`scripts/app_manager.py`) wraps the shared [crosspad-apps](https://github.com/CrossPad/crosspad-apps) core: it downloads `crosspad_app_manager.py` anonymously over HTTPS (with a `gh api` fallback), caches it in `scripts/.cache/` and re-checks upstream at most hourly. The site documents only the platform-idf front end (`idf.py app-*`, https://crosspad.app/docs/reference/cli/) and, for this repo, `new`; the PC commands are:

```bash
python3 scripts/app_manager.py list              # List available apps
python3 scripts/app_manager.py install mixer     # Install an app
python3 scripts/app_manager.py remove mixer      # Remove an app
python3 scripts/app_manager.py update --all      # Update all installed apps
python3 scripts/app_manager.py sync              # Sync manifest with disk
python3 scripts/app_manager.py new my-app --name "My App"   # New app from the template
python3 scripts/app_manager.py                   # Launch interactive TUI
```

**After install/remove:** `cmake -B build -G Ninja && cmake --build build` (CMake reconfigure picks up new sources).

### Platform Abstraction Pattern

crosspad-core defines portable interfaces; this repo provides PC implementations:

| Interface | PC Implementation | Notes |
|---|---|---|
| `IClock` | `PcClock` (std::chrono) | |
| `IEventBus` | core's `FreeRtosEventBus` | `post*()` queued to a dispatch task, `send*()` synchronous |
| `ILedStrip` | `PcLedStrip` | 16 virtual RGB pixels, readable via `pc_get_led_color()` |
| `IMidiOutput` | `PcMidi` / core's `NullMidiOutput` | Swappable at runtime via `getPlatformServices().setMidiOutput()` |
| `IAudioOutput` | `PcAudioOutput` / core's `NullAudioOutput` | Swappable via `getPlatformServices().setAudioOutput()`; OUT2 via `pc_platform_set_audio_output_2()` |
| `ISynthEngine` | `MlPianoSynth` | FM synth, swappable via `getPlatformServices().setSynthEngine()` |
| `IKeyValueStore` | `PcKeyValueStore` | `~/.crosspad/preferences.json` (`%USERPROFILE%\.crosspad\` on Windows) |
| `IGuiPlatform` | `PcGuiPlatform` | Display dimensions, update notifications |

Singletons accessed via `crosspad::getPadManager()`, `crosspad::getEventBus()`, etc. — initialized in `pc_platform_init()`.

### Platform Capabilities

crosspad-core provides a bitflag-based capability query system (`crosspad/platform/PlatformCapabilities.hpp`). Platforms declare what they support; apps query at runtime instead of null-checking interface pointers.

**Available flags** (`enum class Capability : uint32_t`): `Midi`, `AudioOut`, `AudioIn`, `Synth`, `Pads`, `Leds`, `Encoder`, `Display`, `Persistence`, `Vibration`, `WiFi`, `Bluetooth`, `Usb`, `Imu`, `Stm32`, `Sequencer`, `Http`.

**API:**
- `setPlatformCapabilities(caps)` — set all flags at once (call during init)
- `addPlatformCapability(cap)` / `removePlatformCapability(cap)` — modify at runtime
- `hasCapability(cap)` — true if ALL specified flags are present
- `hasAnyCapability(caps)` — true if ANY specified flag is present

**PC sets:** base caps (Pads, Leds, Encoder, Display, Persistence) in `pc_platform_init()`. Midi/AudioOut/AudioIn/Synth are added by core's `PlatformServices` setters as `crosspad_app_init()` registers those devices; `Usb` comes and goes with the serial link.

### App System

Apps are registered via crosspad-core's `AppRegistry` using static `AppRegistrar` constructors. The PC `App` class (`src/pc_stubs/PcApp.hpp`) is a lightweight wrapper — no sequencer, CLI, or kit loader — supporting lifecycle (`start`/`pause`/`resume`/`destroyApp`) and launcher integration. Apps receive pad/MIDI events through the EventBus → AppManagerBase → IApp callback chain.

**Adding a new app:** use `REGISTER_APP(Name, …)` (or define `void _register_<Name>_app()`) in a `.cpp` under `src/apps/`. At configure time `cmake/generate_registry.cmake` scans `src/apps/` for both and writes `build/app_registry_init.cpp`, whose `AppRegistry_InitAll()` `crosspad_app_init()` calls — no hand-written call needed. Reconfigure after adding the file.

### Event & Input Flow

```
Pad click (crosspad-gui VirtualPadGrid), pad key or MIDI NoteOn → PadManager::handlePadPress()
  ├→ PadLedController (LED animation)
  ├→ IEventBus::postPadPressed() → subscribed apps' onPadPressed()
  └→ IPadLogicHandler::onPadPressed() (if active logic set)

MIDI CC1 → Stm32EmuWindow::handleEncoderCC() → encoder rotation
MIDI CC64 → encoder button press
MIDI SysEx → Stm32MessageHandler → PadManager LED updates
```

Host input (`src/stm32_emu/`): mouse click on a pad; mouse wheel turns the encoder, middle click presses it; Space or Ctrl is the power button (tap = back, hold 0.5 s = drawer); V toggles the volume overlay; Escape goes to the launcher. With keyboard capture on, `1234 / QWER / ASDF / ZXCV` are the pad rows top to bottom — V is taken by the volume overlay, so pad 4 has no key. GLOBAL capture hooks the keyboard on Windows only. `docs/instructions.md` (the in-app Help text) has the user-facing version.

### Audio Pipeline

```
App noteOn/noteOff → MlPianoSynth (ISynthEngine, registered via PlatformServices::setSynthEngine)
  → PcAudioModule thread: process() → AudioMixerEngine (IN1, IN2, Synth, Sampler channels; routing to OUT1/OUT2)
      (without the crosspad-mixer app: a plain node chain of synth, sampler and pitched nodes)
  → PcAudioOutput::write() (ring buffer)
  → RtAudio callback: ring buffer → output device (WASAPI on Windows)
  → VU meter timer: pcAudio.getOutputLevel() → crosspad_gui::vu_set_levels()
```

## Compatibility with ESP32-S3

This simulator must stay compatible with the ESP32-S3 firmware ([platform-idf](https://github.com/CrossPad/platform-idf)). Key constraints:

- **App interface:** All apps implement crosspad-core's `IApp` interface (business-logic callbacks: `onNoteOn`, `onPadPressed`, etc.). The PC `App` class can be simpler than ESP32's (no sequencer/CLI), but must support the same lifecycle and AppRegistry pattern.
- **PadManager is the single source of truth** for pad state, note mapping, and LED coordination. Always route pad events through PadManager, not directly to apps.
- **Settings persistence** uses `IKeyValueStore` abstraction — ESP32 uses NVS, PC uses filesystem. `CrosspadSettings` singleton must be loaded/saved through this interface.
- **crosspad-gui components** are shared — launcher, status bar, styles. The PC simulator sets `IGuiPlatform` for display dimensions and hooks.
- **LCD resolution** is 320x240 (`CP_LCD_HOR_RES`/`CP_LCD_VER_RES`), matching hardware. The SDL window is larger (490x714, `Stm32EmuWindow::WIN_W/WIN_H`) to include the emulator body.

### Remote Control Server

The simulator includes a built-in TCP server (`src/remote/RemoteControl.cpp`) on `localhost:19840` for external automation. Started automatically with the simulator; it prints `[Remote] Listening on 127.0.0.1:19840`, or `[Remote] Failed to bind port 19840` when another copy holds the port. Protocol: newline-delimited JSON.

Commands include `ping`, `screenshot`, `click`, `pad_press`/`pad_release`, `power_click`/`power_hold`/`power_press`/`power_release`, `encoder_rotate`/`encoder_press`/`encoder_release`, `key`, `stats`, `settings_get`/`settings_set`, `midi_note_on`/`midi_note_off`, `kit_list`/`kit_load`/`kit_status`, audio device and level commands — the dispatch in `RemoteControl.cpp` is the full list.

Used by the [crosspad-mcp](https://github.com/CrossPad/crosspad-mcp) simulator tools.

## Important Notes

- crosspad-core and crosspad-gui use `idf_component_register()` in their own CMakeLists.txt (ESP-IDF build system). This project bypasses that by globbing their sources in the top-level CMakeLists.txt.
- Third-party code (LVGL, RtMidi, RtAudio, ml_synth) is compiled with warnings suppressed (`/w` on MSVC). Project code uses default warning levels.
- `LV_USE_FS_WIN32` is enabled with driver letter `'C'` for Windows file system access (`LV_USE_FS_POSIX` with `'C'` elsewhere).
- FreeRTOS heap is 32 MB (`config/FreeRTOSConfig.h`). The MSVC-MingW port uses Windows threads under the hood.
- FetchContent pulls ArduinoJson 7.3.0 (required by crosspad-core settings), RtMidi 6.0.0, RtAudio 6.0.0, SimpleBLE v0.7.3 with fmt 11.0.2 (when `USE_BLE`), and Catch2 v3.5.0 (tests).
- Platform repos stay thin: logic that could run on another platform goes to crosspad-core, UI to crosspad-gui (https://crosspad.app/docs/concepts/where-code-belongs/, https://crosspad.app/docs/concepts/manifesto/).
