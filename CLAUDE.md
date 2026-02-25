# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

CrossPad PC — a desktop simulator for the CrossPad embedded device. Runs the same LVGL GUI + crosspad-core/crosspad-gui libraries on desktop via SDL2, with MIDI I/O (RtMidi), audio output (RtAudio/WASAPI), and an STM32 hardware emulator window (4x4 pad grid + rotary encoder). Used for rapid development before deploying to ESP32-S3 hardware.

## Build

Windows with MSVC (Visual Studio 2022 Community):
```
build.bat
```
This calls `vcvarsall.bat x64`, then cmake+ninja with vcpkg toolchain. Enables FreeRTOS by default. Output: `bin/main.exe`.

Manual build (incremental — without wiping build dir):
```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Run: `bin/main.exe`

**Dependencies:** SDL2 via vcpkg (`vcpkg install sdl2:x64-windows`), vcpkg at `C:\vcpkg`.

### CMake Options

| Option | Default | Description |
|---|---|---|
| `USE_FREERTOS` | OFF | FreeRTOS multitasking (recommended ON for full app init) |
| `USE_MIDI` | ON | MIDI I/O via RtMidi (FetchContent) |
| `USE_AUDIO` | ON | Audio output via RtAudio + FM synth (FetchContent) |
| `LV_USE_DRAW_SDL` | OFF | SDL GPU-accelerated drawing |
| `LV_USE_LIBPNG` | OFF | PNG decoding |
| `LV_USE_LIBJPEG_TURBO` | OFF | JPEG decoding |
| `LV_USE_FFMPEG` | OFF | Video playback |
| `LV_USE_FREETYPE` | OFF | FreeType font rendering |
| `ASAN` | OFF | AddressSanitizer (Debug, non-MSVC only) |

### Compile Defines

The `main` target gets: `PLATFORM_PC=1`, `USE_LVGL=1`, `CP_LCD_HOR_RES=320`, `CP_LCD_VER_RES=240`, plus `USE_MIDI=1` and `USE_AUDIO=1` when those options are ON.

## Architecture

### Entry Points & Init Flow

- **Standard mode** (`USE_FREERTOS=OFF`): `src/main.cpp` — single-threaded `lv_timer_handler()` loop. Note: `crosspad_app_init()` is currently commented out in this mode.
- **FreeRTOS mode** (`USE_FREERTOS=ON`): `src/freertos_main.cpp` — creates LVGL task (priority 1), calls `crosspad_app_init()`, then starts scheduler.

Both modes share `crosspad_app_init()` in `src/crosspad_app.cpp`, which orchestrates the full init sequence:
1. `pc_platform_init()` — creates singletons (EventBus, Clock, PadManager, LedController, Settings, GuiPlatform)
2. `stm32Emu.init()` — builds emulator window, returns 320x240 LCD container
3. MIDI setup — auto-connect to "CrossPad" port, route NoteOn/Off → PadManager, CC → Encoder, SysEx → Stm32MessageHandler
4. Audio setup — RtAudio init, FM synth thread, VU meter timer
5. App registration — enumerate `AppRegistry`, create `App` wrappers
6. `initStyles()` + `LoadMainScreen()` — crosspad-gui theme and launcher

### Source Layout

```
src/
  main.cpp / freertos_main.cpp  — entry points
  crosspad_app.cpp              — shared init (MIDI, audio, apps, launcher)
  hal/hal.c                     — SDL2 HAL (display 490x660, mouse, keyboard, mousewheel)
  stm32_emu/                    — device body visualization
    Stm32EmuWindow.cpp          — LCD (320x240), encoder, pad grid layout
    EmuEncoder.cpp              — rotary encoder knob (mouse wheel + middle click)
    EmuPadGrid.cpp              — 4x4 clickable pads with LED color readback
  midi/PcMidi.cpp               — RtMidi wrapper, IMidiOutput impl, auto-connect
  audio/
    PcAudio.cpp                 — RtAudio/WASAPI output, AudioRingBuffer, peak metering
  synth/MlPianoSynth.cpp        — ISynthEngine impl wrapping ML_SynthTools FM engine
  apps/ml_piano/                — ML Piano app (pad grid, preset selector, param controls)
  pc_stubs/
    PcPlatformStubs.cpp         — PC impls: PcClock, PcLedStrip, PcKeyValueStore, PcGuiPlatform, etc.
    PcEventBus.cpp              — synchronous event dispatch (vs. ESP-IDF's async queue)
    PcApp.cpp                   — lightweight App class for launcher (no sequencer/CLI)
    pc_platform.h               — public API: pc_platform_init(), set_midi/audio/synth
lib/ml_synth/                   — vendored ML_SynthTools FM synth engine
```

### Submodules

- **crosspad-core**: Portable C++ library — AppRegistry, IEventBus, PadManager, PadLedController, CrosspadSettings (IKeyValueStore), Stm32MessageHandler, platform interfaces (IClock, IMidiOutput, ILedStrip, IAudioOutput, ISynthEngine). Originally an ESP-IDF component; sources are listed manually in CMakeLists.txt.
- **crosspad-gui**: LVGL UI components — theme, styles, launcher, status bar, widgets (keypad buttons, spinbox, radial menu, VU meter, file explorer, DFU panel, modals/toasts). Originally an ESP-IDF component; sources listed manually.
- **lvgl**: LVGL v9.x graphics library
- **FreeRTOS**: FreeRTOS Kernel (MSVC-MingW port on Windows, GCC POSIX on Linux/Mac)

### Platform Abstraction Pattern

crosspad-core defines portable interfaces; this repo provides PC implementations:

| Interface | PC Implementation | Notes |
|---|---|---|
| `IClock` | `PcClock` (std::chrono) | |
| `IEventBus` | `PcEventBus` | Synchronous dispatch (not queued) |
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

This simulator must stay compatible with the ESP32-S3 target (`C:\Users\Mateusz\GIT\ESP32-S3`). Key constraints:

- **App interface:** All apps implement crosspad-core's `IApp` interface (business-logic callbacks: `onNoteOn`, `onPadPressed`, etc.). The PC `App` class can be simpler than ESP32's (no sequencer/CLI), but must support the same lifecycle and AppRegistry pattern.
- **PadManager is the single source of truth** for pad state, note mapping, and LED coordination. Always route pad events through PadManager, not directly to apps.
- **Settings persistence** uses `IKeyValueStore` abstraction — ESP32 uses NVS, PC uses filesystem. `CrosspadSettings` singleton must be loaded/saved through this interface.
- **crosspad-gui components** are shared — launcher, status bar, styles. The PC simulator sets `IGuiPlatform` for display dimensions and hooks.
- **LCD resolution** is 320x240 (`CP_LCD_HOR_RES`/`CP_LCD_VER_RES`), matching hardware. The SDL window is larger (490x660) to include the emulator body.

## Important Notes

- crosspad-core and crosspad-gui use `idf_component_register()` in their own CMakeLists.txt (ESP-IDF build system). This project bypasses that by listing their sources manually in the top-level CMakeLists.txt.
- Third-party code (LVGL, RtMidi, RtAudio, ml_synth) is compiled with warnings suppressed (`/w` on MSVC). Project code uses default warning levels.
- `LV_USE_FS_WIN32` is enabled with driver letter `'C'` for Windows file system access.
- FreeRTOS heap is 512 MB (desktop simulation). The MSVC-MingW port uses Windows threads under the hood.
- FetchContent pulls ArduinoJson 7.3.0 (required by crosspad-core settings), RtMidi 6.0.0, and RtAudio 6.0.0.

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
