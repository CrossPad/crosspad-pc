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

- `scripts/run.sh` builds (Linux/macOS, Ninja, Debug) when `bin/CrossPad` or `build/` is missing or any source under `src/`, `lib/crosspad-core/src` or `lib/crosspad-gui/src` is newer than `bin/CrossPad`, then launches it. CMake also has a `run` target.
- `bin/CrossPad --versions` prints one `APPVER: <component> id=… commit=… ref=… dirty=…` line per core / gui / app it was built with, then `APPVER: end count=N`, and exits — the simulator's answer to the firmware's APP_VERSIONS, used by the app manager.
- Windows: `build.bat` calls Visual Studio 2022 **Community**'s `vcvarsall.bat x64`, **wipes `build/` every time**, then configures with the vcpkg toolchain (`C:/vcpkg/scripts/buildsystems/vcpkg.cmake`). For incremental builds run the two cmake lines by hand from an x64 Native Tools prompt. A post-build step copies `SDL2.dll` (from the vcpkg tree) next to the exe.
- Linux: `libdbus-1-dev` is only needed for `USE_BLE` (SimpleBLE); without it configure with `-DUSE_BLE=OFF`. `libpipewire-0.3-dev` is optional: without it CMake prints "PipeWire dev headers not found" and the virtual sinks fall back to `pactl` at runtime.
- Platforms: the simulator runs on Linux, Windows and macOS. Serial ports (`src/uart/PcUart.cpp`: the virtual USB/UART jack, the serial monitor app, `Capability::Usb`) are Windows-only; Linux and macOS get an empty stub. On macOS, MIDI (CoreMIDI) and audio (CoreAudio) are not well tested; `docs/building-macos.md` lists what works there and what is missing.
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
| `CROSSPAD_FW_KIT` | `full` | Firmware kit (`config/fw_kits/<kit>.json`): which apps reach the launcher and the binary's name (§ Firmware kits) |
| `ASAN` | unset | AddressSanitizer (Debug, non-MSVC only). Not declared anywhere: only `if (ASAN)` reads it, so pass `-DASAN=ON` |

### Compile Defines

The `CrossPad` target gets: `PLATFORM_PC=1`, `USE_LVGL=1`, `USE_FREERTOS=1`, `CP_LCD_HOR_RES=320`, `CP_LCD_VER_RES=240`, `CROSSPAD_REGISTER_APP_NAMED_FUNCTIONS=1`, the build's `CROSSPAD_CORE_REV` / `CROSSPAD_GUI_REV` / `CROSSPAD_PC_VERSION` and the kit's `CROSSPAD_FW_KIT_*` / `CROSSPAD_FW_STANDALONE_APP` / `CROSSPAD_FW_TOP_BAR`, plus `USE_MIDI=1`, `USE_AUDIO=1`, `USE_BLE=1`, `USE_VIRTUAL_AUDIO=1` when those options are ON, and `CROSSPAD_DEV_BUILD=1` when `BUILD_TESTING` is ON. On Linux the link wraps `lv_async_call` (`CROSSPAD_WRAP_LV_ASYNC=1`).

### Core / GUI version gate

`src/crosspad_deps.hpp` declares the crosspad-core / crosspad-gui versions this simulator was written against (today core 2.16.0, gui 2.7.8; major mismatch is `#error`, minor/patch drift a `#warning`). Bump it together with the `lib/` submodules, after building and running against them. Keep the pins in step with what platform-idf's `main` uses.

## Architecture

### Entry Points & Init Flow

`src/freertos_main.cpp` is the only entry point: `main()` handles `--versions`, then the firmware-slot arguments (`--fw-slot`, `--recovery`, and `--boot`, which starts the boot slot and ends there; § Firmware kits), installs the shutdown signal handlers, creates the LVGL task (priority 1) and starts the scheduler. The LVGL task opens the SDL window, calls `crosspad_app_init()` and runs the LVGL loop on `crosspad_app_tick()`; once that reports the boot finished (the launcher is built) it attaches the board's screen-wide swipes to the mouse and starts the remote control server (`remote::start`).

`crosspad_app_init()` in `src/crosspad_app.cpp` runs on the LVGL thread and hands the slow part to a worker:
1. `pc_fw::boot()` — which firmware slot this binary is, and that firmware's own settings in place before they are read
2. `pc_platform_init()` — creates singletons (EventBus, Clock, PadManager, LedController, Settings, GuiPlatform); the pad animation tick
3. `stm32Emu.init()` — builds emulator window, returns 320x240 LCD container; the saved SD card folder is mounted
4. `boot_splash_begin()` — crosspad-gui's cold-boot splash, the first thing on the panel (~2 s, the emulated pads go along); `CROSSPAD_NO_SPLASH=1` in the environment skips it, as the GUI test harness does
5. `MidiInputHandler` (registered as the `IMidiOutput`) and `Stm32MessageHandler`
6. The `boot_dev` worker task brings the devices up behind the splash: MIDI (auto-connect to the saved ports, else the first whose name contains "CrossPad", port 0 if none; NoteOn/Off → PadManager, CC → Encoder, SysEx → Stm32MessageHandler; `CROSSPAD_NO_MIDI=1` touches no port), BLE MIDI, audio (RtAudio devices, virtual sinks, FM synth, mixer engine, `PcAudioModule`)
7. Then `crosspad_app_tick()` on the LVGL thread runs `boot_finish()`: the MIDI reconnect timer, app registration (`AppRegistry_InitAll()`, generated at configure time, § App System), `initStyles()`, the orchestrator's app list, and `LoadMainScreen()` — the home screen `CrosspadSettings::launcherStyle` picks (Settings → Display: the pad map, the default; the icon grid; the list)

### Source Layout

```
src/
  freertos_main.cpp             — entry point (LVGL task + scheduler)
  crosspad_app.cpp              — shared init (MIDI, audio, apps, launcher)
  crosspad_deps.hpp             — required crosspad-core / crosspad-gui versions
  hal/hal.c                     — SDL2 HAL (display, mouse as the touch panel -- its own driver, so crosspad-gui's touch_gesture can wrap it --, keyboard, mousewheel)
  freertos/                     — FreeRTOS port glue (POSIX, Win32)
  stm32_emu/                    — device body: LCD, encoder, pads, jack panel, SD slot, keyboard capture
  midi/                         — PcMidi (RtMidi, auto-connect), PcBleMidi (SimpleBLE)
  audio/                        — RtAudio output/input, audio module, sampler/ (core's SamplerEngine: kits, second kit, pitched; its README),
                                  pipewire/ (Linux virtual-audio backend, see docs/virtual-audio.md), virtual/
  synth/MlPianoSynth.cpp        — ISynthEngine impl wrapping ML_SynthTools FM engine
  sequencer/, song/             — PC ports of the pattern sequencer and core's song engine
  sampletools/                  — the Recorder's and the Slicer's platform port
  fw/                           — firmware kits, the card's library and slots A/B (§ Firmware kits)
  uart/, updater/               — PcUart (serial ports, Windows COM API; stub elsewhere) and the self-updater
  remote/RemoteControl.cpp      — TCP server (localhost:19840, `CROSSPAD_REMOTE_PORT`) for MCP integration
  remote/ControlJson.cpp        — JSON of the verbs the board answers too (§ Remote Control Server)
  pc_stubs/
    PcPlatformStubs.cpp         — PC impls: PcClock, PcLedStrip, PcKeyValueStore, PcGuiPlatform, etc.
    PcApp.hpp                   — the App class, which is crosspad-gui's App (AppBase)
    FreeRtosEventBus.cpp        — the IEventBus
    PcSubprocess.cpp            — child processes through posix_spawnp(), never fork() (a forked child of this many-threaded process could hang on an allocator lock)
    PcHttpClient.cpp            — HTTP for the App Store / updater
    pc_platform.h               — public API: pc_platform_init(), OUT2/input setters, LED readback, PC-only settings (update check, USB autoconnect, SD path)
  apps/settings, citest, update, firmware — built-in apps
  apps/crosspad-*/              — installed apps (submodules)
lib/ml_synth/                   — vendored ML_SynthTools FM synth engine
lib/crosspad-core/              — submodule: portable C++ library
lib/crosspad-gui/               — submodule: shared LVGL UI components
lvgl/, FreeRTOS/                — upstream submodules
config/fw_kits/                 — firmware kits (§ Firmware kits)
scripts/
  app_manager.py                — Python app manager (wrapper for crosspad-apps core)
  run.sh                        — Smart build+run script
  sim_start_stress.py           — start an isolated instance N times, classify each start
  make_fw_package.py            — a kit's binary as a firmware package on the card
tests/                          — Catch2 tests (ctest labels: gui, flaky)
docs/                           — design notes; instructions.md is the in-app Help text
```

### Submodules

**Shared libraries** (in `lib/`):

- **crosspad-core**: Portable C++ library — AppRegistry, IEventBus, PadManager, PadLedController, CrosspadSettings (IKeyValueStore), Stm32MessageHandler, platform interfaces (IClock, IMidiOutput, ILedStrip, IAudioOutput, ISynthEngine). Originally an ESP-IDF component; sources are auto-discovered (`file(GLOB_RECURSE)`) in CMakeLists.txt, minus `SyncEventBus` and `Stm32Manager`.
- **crosspad-gui**: LVGL UI components — theme, styles, launcher, status bar, widgets (keypad buttons, spinbox, radial menu, VU meter, file explorer, DFU panel, modals/toasts). Originally an ESP-IDF component; `.cpp` and `.c` sources (fonts, icons) auto-discovered.
- **lvgl**: LVGL v9.x graphics library
- **FreeRTOS**: FreeRTOS Kernel (MSVC-MingW port on Windows, GCC POSIX on Linux/Mac)

**Installable apps** (in `src/apps/crosspad-*/`, managed by app manager):

- **crosspad-appstore**: the App Store (PC only). It hides itself from its own list, so it cannot remove itself, and release builds (`BUILD_TESTING=OFF`) leave it out
- **crosspad-mixer**, **crosspad-piano**, **crosspad-instructions**, **crosspad-serial-monitor** (needs serial ports, so Windows only), **crosspad-sampler**: Installable via `python3 scripts/app_manager.py install <name>` or via the App Store UI
- **crosspad-sequencer**, **crosspad-song**, **crosspad-arrange**, **crosspad-pad-mixer**, **crosspad-recorder**, **crosspad-slicer**, **crosspad-pitchplay**, **crosspad-dawcontrol**: the board's apps, with the platform services they need wired here -- the pattern sequencer and metronome (`src/sequencer/PcSequencer.cpp`, scenes in `<sdcard_path>/crosspad/sequences.json`), core's song engine (`src/song/PcSongEnginePort.cpp`, projects in `<sdcard_path>/SONGS`, bounces from the mixer's third "Rec (pads)" bus), a song's second kit track on an engine of its own (`src/audio/sampler/PcSecondKit.cpp`, core `ISecondKit`, the "Kit 2" channel), the sample tools (`src/sampletools/`), per-pad levels (`PcPadMix` in `PcSamplerPort.cpp`) and `stm32MessageHandler` for DAW Control (no hub on PC, so no launch buttons)
- **crosspad-doom**, **crosspad-pong**, **crosspad-pianotiles**: games, each the app of its firmware kit (§ Firmware kits); a kit-only app is left out of the other kits' launchers

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
| `IEventBus` | `FreeRtosEventBus` (src/pc_stubs) | `post*()` queued to a dispatch task, `send*()` synchronous |
| `ILedStrip` | `PcLedStrip` | 16 virtual RGB pixels, readable via `pc_get_led_color()` |
| `IMidiOutput` | core's `MidiInputHandler` → `PcMidi` / `PcBleMidi` | `MidiInputHandler` is what `setMidiOutput()` registers; it fans out to `PcMidi` (`setUsbOutput`) and `PcBleMidi` (`setBleOutput`) |
| `IAudioOutput` | `PcAudioOutput` behind `PcAudioModule` | The module is registered with `getPlatformServices().setAudioModule()`; its output streams wrap `PcAudioOutput` (OUT1, OUT2). `setAudioOutput()` is not used on PC; OUT2 via `pc_platform_set_audio_output_2()` |
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

**PC sets:** base caps (Pads, Leds, Encoder, Display, Persistence) in `pc_platform_init()`. Midi, Synth, AudioOut (via `setAudioModule`), Http and Bluetooth are added by core's `PlatformServices` setters as the devices register; `AudioIn` is not set on PC (`pc_platform_set_audio_input()` only stores the pointer); `Usb` follows the serial link (Windows only).

### App System

Apps are registered with crosspad-core's `AppRegistry` through the `_register_<Name>_app()` functions `REGISTER_APP` / `REGISTER_APP_PL` define — no static registrar objects. The `App` class is crosspad-gui's (`AppBase` CRTP; `src/pc_stubs/PcApp.hpp` only includes it), the same as the board's: `start()` builds the UI and pushes the app's pad logic, `destroyApp()` pops it, and crosspad-gui's `AppOrchestrator` runs the launcher. Apps get pads through that pad logic (`IPadLogicHandler`) and everything else through `IEventBus` subscriptions.

**Adding a new app:** use `REGISTER_APP(Name, …)`, `REGISTER_APP_PL(Name, …)` (with pad logic, as the app template does), or define `void _register_<Name>_app()` in a `.cpp` under `src/apps/`. At configure time crosspad-core's `cmake/crosspad_app_registry.cmake` (`crosspad_generate_app_registry()`) scans `src/apps/` for all three and writes `build/app_registry_init.cpp` (a firmware kit's own apps only; no App Store with `BUILD_TESTING=OFF`), whose `AppRegistry_InitAll()` `crosspad_app_init()` calls — no hand-written call needed. Reconfigure after adding the file.

### Event & Input Flow

```
Pad click (crosspad-gui VirtualPadGrid), pad key or MIDI NoteOn → PadManager::handlePadPress()
  ├→ PadLedController (LED animation)
  ├→ IEventBus::postPadPressed() → subscribers
  └→ IPadLogicHandler::onPadPress() (the active pad logic)

MIDI CC1 → Stm32EmuWindow::handleEncoderCC() → encoder rotation
MIDI CC64 → encoder button press
MIDI SysEx → Stm32MessageHandler → PadManager LED updates
```

Host input (`src/stm32_emu/`): mouse click on a pad; mouse wheel turns the encoder, middle click presses it; Space or Ctrl is the power button (tap = back, hold 0.5 s = drawer); V toggles the volume overlay; Escape goes to the home screen (in a `standalone` kit it is the power click). With keyboard capture on, `1234 / QWER / ASDF / ZXCV` are the pad rows top to bottom — V is taken by the volume overlay, so pad 4 has no key, except in GLOBAL mode (Windows only), where the low-level hook maps V to pad 4 and the volume key is unreachable. OFF turns only the pad keys off; the shortcuts still work. `docs/instructions.md` (the in-app Help text) has the user-facing version.

### Audio Pipeline

```
App noteOn/noteOff → MlPianoSynth (ISynthEngine, registered via PlatformServices::setSynthEngine)
  → PcAudioModule thread: process() → AudioMixerEngine (Input 1, Input 2, Synth, Sampler, Pitched, Click, Song, Preview, Kit 2 channels; buses OUT1, OUT2 and the "Rec (pads)" capture bus)
      (without the crosspad-mixer app: a plain node chain of synth, sampler, pitched and second-kit nodes)
  → PcAudioOutput::write() (ring buffer)
  → RtAudio callback: ring buffer → output device (WASAPI on Windows)
  → VU meter timer: pcAudio.getOutputLevel() → crosspad_gui::vu_set_levels()
```

A generator put on the node chain with `IAudioModule::addNode()` (DOOM's
sound effects) plays on OUT1 also when the mixer engine renders the buses.

### Firmware kits, the library and slots A/B

The board's firmware library and A/B switch, played by simulator binaries
(`src/fw/`). A kit (`config/fw_kits/<kit>.json`, `-DCROSSPAD_FW_KIT=<kit>`)
is one binary: `bin/CrossPad` for `full`, `bin/CrossPad-<kit>` otherwise
(build dir `build-<kit>`, e.g. `cmake -B build-doom -G Ninja
-DCROSSPAD_FW_KIT=doom -DBUILD_TESTING=OFF`). Its descriptor
(`src/fw/PcFwDesc.cpp`, `CPFI-PC1;kit=…`) is what a package is judged by.
`python3 scripts/make_fw_package.py bin/CrossPad-doom` puts it on the mounted
card as `crosspad/firmware/<kit>-<version>/` (`CrossPad.pc`, `icon.bin` from
`config/fw_kits/icons/<kit>.png`, `manifest.json` with the kit's text); the
Firmware app's Library installs it into the slot not running and switches,
which starts that slot's binary (`--fw-slot=A|B`) and ends this process.

- Slots live in `<profile>/fw/slots.json` (+ `slot_a|slot_b/CrossPad.pc`); a
  binary started by hand is slot A, "flashed over USB".
- `bin/CrossPad --boot` is the power button and bootloader: it starts the boot
  slot, and rolls back one that was started on trial and never confirmed
  itself (crashed, killed) -- the card then offers *Try this slot again* /
  *Reinstall*, and the slot booted instead shows a toast.
- A switched-into slot confirms itself 5 s after its boot; a switch away from
  an unconfirmed one is refused, as on the board.
- Each firmware keeps its own `preferences.json` (`fw/settings/<package>.json`,
  saved on a switch, taken back by the slot started); `device_preferences.json`
  (card, audio devices) is shared, as the board shares its list.
- The assets are shared too: a slot's binary gets the starting one's through
  `CROSSPAD_ASSETS`, as both board slots read one assets partition.
- Pad 16 held through the boot (or `--recovery`) switches to the slot holding
  the `full` kit, before the firmware's own app starts.
- A kit with `standalone` starts into that app with no launcher (Escape is the
  power click there); `"top_bar": false` gives it the whole panel.

Switching on Windows goes through `CreateProcessW` and is not tried on a
Windows machine yet.

## Compatibility with ESP32-S3

This simulator must stay compatible with the ESP32-S3 firmware ([platform-idf](https://github.com/CrossPad/platform-idf)). Key constraints:

- **App interface:** All apps implement crosspad-core's `IApp` interface (business-logic callbacks: `onNoteOn`, `onPadPressed`, etc.). The PC `App` class can be simpler than ESP32's (no sequencer/CLI), but must support the same lifecycle and AppRegistry pattern.
- **PadManager is the single source of truth** for pad state, note mapping, and LED coordination. Always route pad events through PadManager, not directly to apps.
- **Settings persistence** uses `IKeyValueStore` abstraction — ESP32 uses NVS, PC uses filesystem. `CrosspadSettings` singleton must be loaded/saved through this interface.
- **crosspad-gui components** are shared — launcher, status bar, styles. The PC simulator sets `IGuiPlatform` for display dimensions and hooks.
- **LCD resolution** is 320x240 (`CP_LCD_HOR_RES`/`CP_LCD_VER_RES`), matching hardware. The SDL window is larger (490x714, `Stm32EmuWindow::WIN_W/WIN_H`) to include the emulator body.

### Remote Control Server

The simulator includes a built-in TCP server (`src/remote/RemoteControl.cpp`) on `localhost:19840` (`CROSSPAD_REMOTE_PORT` overrides) for external automation. Started automatically once the boot has built the launcher (the splash is up before it; a client connecting earlier gets refused, so poll `ping`). Protocol: newline-delimited JSON, one object with a `cmd` per line (anything else gets `malformed request`). It prints `[Remote] Listening on 127.0.0.1:<port>`. If the port cannot be bound (another simulator owns it) the window title reads `NO CONTROL PORT (<port> busy)` and stdout/stderr say `[Remote] !!! NO CONTROL PORT`; `CROSSPAD_REMOTE_REQUIRED=1` makes that exit code 3. Up to 8 clients at once, each on its own thread; the LVGL thread never waits on a socket (a command not answered in 10 s returns `timeout` and is skipped; `ping` and the `audio_*` list/set verbs run on the client's thread, never the LVGL one). `screenshot` reads LVGL's own framebuffer, not the SDL back buffer (which came back black now and then after a present).

Commands include `ping`, `screenshot`, `click`, `drag` (`x1`,`y1`,`x2`,`y2` in `lcd` space by default, `ms`, optional `steps`, `rest_ms` held still before release -- longer than LVGL's momentum window lets go without a flick; the reply comes at once, the drag takes `ms`+`rest_ms`), `top_bar` (which of the transport/status bars is the page, the row's scroll, the drawer, the encoder's focus and where, the last screen-wide swipe), `pad_press`/`pad_release`, `pad_pressure` (`pad`, `pressure` 0-255, the board's `PAD_PRESSURE`), `pad_pan` (`pad`, `value` 0 left .. 64 centred .. 127 right, the board's `PAD_PAN`; the reply's `pan` is what `onPadPan()` got, -64..63), `app_start` (`name`, case-insensitive, the board's `APP_START`), `app_list`, `power_click`/`power_hold`/`power_press`/`power_release`, `encoder_rotate`/`encoder_press`/`encoder_release`, `key`, `stats`, `settings_get`/`settings_set`, `midi_note_on`/`midi_note_off`, `kit_list`/`kit_load`/`kit_status`, `audio_level`, `audio_out_list`/`audio_in_list`/`audio_out_set`/`audio_in_set`, `mix_lvl`, `led_state`, `pitched_status`, `wave_status`, `smpl_peak`, `smpl_loops`, `seq`, `enc_group`, `ui_find`, `citest_run`/`citest_status` — the dispatch in `RemoteControl.cpp` is the full list. The verbs the board answers too (kit_*, led_state, mix_lvl, pitched_status, wave_status, audio_level, pad_*, app_start) read their state through crosspad-core `crosspad/control/`, the same readers the board's CDC verbs use; their JSON is rendered in `src/remote/ControlJson.cpp` and pinned by `tests/test_control_json.cpp` — a reply change there is a protocol change for the MCP.

Used by the [crosspad-mcp](https://github.com/CrossPad/crosspad-mcp) simulator tools.

**Running a test instance beside a user's simulator** (all env, none changes a normal start):

| Variable | Effect |
|----------|--------|
| `CROSSPAD_REMOTE_PORT` | control port (default 19840; never use that for a test) |
| `CROSSPAD_INSTANCE=<id>` | suffix for the PipeWire/Pulse nodes (`crosspad_vin1_<id>`, `crosspad_out_<id>`); defaults to the port when that is not 19840. The pactl fallback's stale-sink cleanup removes only this instance's nodes |
| `CROSSPAD_AUDIO=on\|isolated\|off` | `on` (default without an id): plain names + default-sink takeover. `isolated` (default with an id): real audio, suffixed names, no takeover. `off`: no audio device, no virtual sinks. Anything but `on` with no id uses id `test`. Only the primary `on` instance ever takes the default sink |
| `CROSSPAD_PROFILE_DIR` | replaces `~/.crosspad` (preferences, device_preferences, mixer_state, cache; the SD card path comes from its `device_preferences.json`). The updater's version cache still lives under `$HOME` |
| `CROSSPAD_NO_MIDI=1` | no MIDI ports touched |
| `SDL_VIDEODRIVER=dummy SDL_RENDER_DRIVER=software` | headless; the control port's screenshot still works |

`scripts/sim_start_stress.py N` starts an isolated instance N times and classifies each start OK / HANG / CRASH (gdb stacks for a hang, `--gdb` for a segfault); see its `--help`.
## Important Notes

- crosspad-core and crosspad-gui use `idf_component_register()` in their own CMakeLists.txt (ESP-IDF build system). This project bypasses that by globbing their sources in the top-level CMakeLists.txt.
- Third-party code (LVGL, RtMidi, RtAudio, ml_synth) is compiled with warnings suppressed (`/w` on MSVC). Project code uses default warning levels.
- `LV_USE_FS_WIN32` is enabled with driver letter `'C'` for Windows file system access (`LV_USE_FS_POSIX` with `'C'` elsewhere).
- FreeRTOS heap is 32 MB (`config/FreeRTOSConfig.h`). The MSVC-MingW port uses Windows threads under the hood.
- FetchContent pulls ArduinoJson 7.3.0 (required by crosspad-core settings), RtMidi 6.0.0, RtAudio 6.0.0, SimpleBLE v0.7.3 with fmt 11.0.2 (when `USE_BLE`), and Catch2 v3.5.0 (tests).
- Platform repos stay thin: logic that could run on another platform goes to crosspad-core, UI to crosspad-gui (https://crosspad.app/docs/concepts/where-code-belongs/, https://crosspad.app/docs/concepts/manifesto/).
