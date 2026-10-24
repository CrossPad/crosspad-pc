# The CrossPad simulator on macOS

Status: **builds and runs on macOS** (owner report, 2026-10); not in CI. MIDI
(CoreMIDI) and audio (CoreAudio) are not well tested. Not available on macOS:
serial ports (USB/UART), virtual audio devices, the SD-card folder picker,
self-update and GLOBAL keyboard capture. BLE is unverified.

Build steps: https://crosspad.app/docs/start-here/pc-simulator/ (macOS tab).

## What builds and works (read from the code)

- **No `if(APPLE)` branch is needed for MIDI and audio.** RtMidi 6.0.0 turns
  on `RTMIDI_API_CORE` when `APPLE` is set and links CoreServices, CoreAudio,
  CoreMIDI and CoreFoundation itself. RtAudio 6.0.0 does the same for CoreAudio
  (`RTAUDIO_API_CORE`). PipeWire and PulseAudio are gated to Linux in
  CMakeLists.txt.
- FreeRTOS uses the kernel's **GCC POSIX port** (`port.c` +
  `utils/wait_for_event.c`), which builds on macOS via pthreads.
- **No unguarded Linux-only system headers.** Every `<pulse/*>`, `<spa/*>`,
  `<pipewire-*>` include lives in a file compiled only under
  `CROSSPAD_PIPEWIRE_OK` / `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")`, or behind
  `#ifdef __linux__` (`PulseMonitorCapture.cpp`).
- **The audio device selector carries no platform `#ifdef`.** Linux-only
  corners (PipeWire labels, extra sinks, default-sink guard, monitor capture)
  are behind `crosspad_pc::audio_platform`, whose non-Linux path is a no-op, so
  on macOS the selector runs on plain RtAudio (CoreAudio) devices.
- The exe path comes from `_NSGetExecutablePath` (`PcPlatformStubs.cpp`), and
  HTTP (App Store, update check) runs `curl` through `posix_spawnp`
  (`PcHttpClient.cpp`, `PcSubprocess.cpp`).

## What is missing on macOS

- **Serial ports.** `src/uart/PcUart.cpp` is a Windows COM backend; every other
  OS gets an empty stub, so the USB/UART jack never connects and the serial
  monitor app has nothing to read. (Linux has the same gap.)
- **Virtual audio devices.** `VirtualSinkFactory` returns `NullSinkManager`
  off Linux; a CoreAudio/BlackHole backend would be new work.
- **SD-card folder picker.** `EmuSdCardSlot::openFolderDialog()` prints "not
  implemented on this platform"; set `sdcard_path` in
  `device_preferences.json` instead.
- **Self-update.** `PcUpdater` has Windows and Linux (AppImage) paths; macOS
  gets the stubs.
- **GLOBAL keyboard capture.** The low-level keyboard hook is Windows-only;
  FOCUS mode works.
- **BLE: unverified.** SimpleBLE has a Darwin backend (Obj-C++ `.mm` sources,
  CoreBluetooth), but this project enables only `C CXX`, so it may need
  `enable_language(OBJCXX)`. Until someone checks, build with `-DUSE_BLE=OFF`
  (the site's macOS command does).
- **No macOS build entry or CI job.** `build.py` / `build.bat` are Windows;
  use the plain `cmake -G Ninja` lines from the site.

## Verify (on a Mac)

- `cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=$(brew --prefix) -DUSE_BLE=OFF`
  then `cmake --build build` produces `bin/CrossPad`.
- `bin/CrossPad` opens the SDL window; `src/audio/sampler/test/run_sim_hil.py audio`
  passes against it (the audio IN/OUT selector).
