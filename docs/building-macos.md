# Building the CrossPad simulator on macOS

Status: **source is portable, the build system is not wired for macOS yet.**
This is the checklist to close that gap. It needs a Mac (or a macOS CI runner)
to develop and verify — the items below are structurally correct but unbuilt on
Apple hardware.

## What already holds (audited 2026-09-11, on Linux)

- **No unguarded Linux-only system headers.** Every `<pulse/*>`, `<spa/*>`,
  `<pipewire-*>` include lives in a file compiled only under
  `CROSSPAD_PIPEWIRE_OK` / `if(CMAKE_SYSTEM_NAME STREQUAL "Linux")`, or behind
  `#ifdef __linux__` (`PulseMonitorCapture.cpp`). None reach a macOS compile.
- **The audio device selector carries no platform `#ifdef`.** Linux-only
  corners (PipeWire labels, extra sinks, default-sink guard, monitor capture)
  are behind `crosspad_pc::audio_platform`, whose non-Linux path is a no-op —
  so on macOS the selector runs on plain RtAudio (CoreAudio) devices.
- **The virtual-sink manager already stubs on non-Linux** (`VirtualSinkFactory`
  returns `NullSinkManager`), so the system-wide-mixer feature is simply absent
  on macOS until a CoreAudio/BlackHole backend is added — the app still runs.
- FreeRTOS uses the **POSIX port** (`freertos_posix_port.c`), which builds on
  macOS via pthreads.

## What is missing (the actual work, on a Mac)

1. **CMake `if(APPLE)` branch.** Mirror the `WIN32` / `Linux` branches:
   - RtAudio: CoreAudio backend (`RTAUDIO_API_CORE`); link `-framework CoreAudio
     -framework CoreFoundation`.
   - rtmidi: CoreMIDI; link `-framework CoreMIDI`.
   - simpleble (BLE): CoreBluetooth; link `-framework Foundation -framework
     CoreBluetooth` (or set `USE_BLE=OFF` for a first build).
   - Do **not** set `RTAUDIO_API_PULSE`, `USE_PIPEWIRE`, or link `pulse` /
     `pulse-simple` — those are the `Linux` branch only.
2. **Dependencies via Homebrew:** `sdl2 sdl2_image libpng jpeg freetype
   ffmpeg ninja cmake` (and `pkg-config`). The `find_package`/`pkg_check_modules`
   calls for these already exist; they just need the brew prefix on the search
   path (`CMAKE_PREFIX_PATH=$(brew --prefix)`).
3. **A macOS build entry.** `build.py` is MSVC/vcpkg-specific and `build.bat` is
   Windows; add a macOS path (plain `cmake -G Ninja` + `ninja`, like the Linux
   `dev-mode.sh`) or extend `build.py` with an `elif sys.platform == "darwin"`.
4. **First build, iterate on the linker errors.** Expect missing frameworks and
   brew include paths — fix as they surface; there is no way to predict them
   exactly from Linux.

## Verify

- `cmake -G Ninja -B build && ninja -C build` produces `bin/CrossPad`.
- `bin/CrossPad` opens the SDL window; `docs/../src/audio/sampler/test/run_sim_hil.py audio`
  passes against it (the audio IN/OUT selector — the portable path already
  covered on Linux).
