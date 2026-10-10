# CrossPad PC

Desktop simulator for the [CrossPad](https://github.com/CrossPad) 16-pad MIDI
controller. It runs the same `crosspad-core` logic and `crosspad-gui` (LVGL 9)
interface as the ESP32-S3 firmware, on top of SDL2 and FreeRTOS's POSIX /
Windows port, with an emulated device body around the 320×240 LCD: 4×4 pad
grid, rotary encoder, power button.

What works on PC:

- The launcher and the installable apps (sampler, mixer, sequencer, song,
  arrange, pad mixer, DAW Control, piano, …)
- MIDI in/out (RtMidi) and BLE MIDI (SimpleBLE)
- Audio out/in (RtAudio); on Linux, OS-visible virtual sinks through PipeWire
  (falls back to `pactl` at runtime) — see [docs/virtual-audio.md](docs/virtual-audio.md)
- A TCP control port on `localhost:19840` used by
  [crosspad-mcp](https://github.com/CrossPad/crosspad-mcp) for screenshots,
  input and settings

## Get the code

```bash
git clone --recursive https://github.com/CrossPad/crosspad-pc.git
cd crosspad-pc
```

Already cloned without `--recursive`:

```bash
git submodule update --init --recursive
```

## Build

### Linux (Debian / Ubuntu)

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
    libsdl2-dev libasound2-dev libpulse-dev libdbus-1-dev
sudo apt install libpipewire-0.3-dev   # optional: native PipeWire virtual sinks
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./bin/CrossPad
```

`scripts/run.sh` rebuilds when sources changed, then launches.

### Windows (MSVC)

Visual Studio 2022 with the C++ workload, and SDL2 from vcpkg at `C:\vcpkg`
(`vcpkg install sdl2:x64-windows`). Then run `build.bat`, or by hand from a
*x64 Native Tools* prompt:

```bat
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build
bin\CrossPad.exe
```

### macOS

The simulator runs, but USB CDC, MIDI and audio are not well tested there.
Notes and open items: [docs/building-macos.md](docs/building-macos.md).

### CMake options

| Option | Default | |
|---|---|---|
| `USE_MIDI` | ON | MIDI I/O via RtMidi |
| `USE_AUDIO` | ON | Audio via RtAudio |
| `USE_BLE` | ON | BLE MIDI via SimpleBLE (needs `libdbus-1-dev` on Linux) |
| `USE_VIRTUAL_AUDIO` | ON | OS-visible virtual audio sinks |
| `USE_PIPEWIRE` | ON | Native PipeWire backend (Linux, needs `libpipewire-0.3-dev`) |
| `BUILD_TESTING` | ON | Catch2 tests |
| `ASAN` | OFF | AddressSanitizer (Debug, non-MSVC) |

## Run

```bash
./bin/CrossPad              # the simulator
./bin/CrossPad --versions   # core, gui and app versions it was built with
```

Pads take mouse clicks or MIDI notes from a port named "CrossPad"; the mouse
wheel turns the encoder; Space (or Ctrl) is the power button.

## Tests

```bash
ctest --test-dir build --output-on-failure -LE gui
```

`-LE gui` skips the tests that need a display.

## Apps

Apps are git submodules in `src/apps/crosspad-*/`, discovered by CMake at
configure time and managed with the shared
[crosspad-apps](https://github.com/CrossPad/crosspad-apps) tooling:

```bash
python3 scripts/app_manager.py list
python3 scripts/app_manager.py install mixer
python3 scripts/app_manager.py remove mixer
```

Reconfigure (`cmake -B build`) after installing or removing an app. To write
one, see the
[crosspad-appstore README](https://github.com/CrossPad/crosspad-appstore#creating-a-crosspad-app).

## Layout

```
src/               simulator: entry point, platform stubs, emulator window,
                   MIDI, audio, BLE, remote control, PC ports of the services
src/apps/          installed apps (submodules) and the built-in ones
lib/crosspad-core  shared logic (submodule)
lib/crosspad-gui   shared LVGL UI (submodule)
lvgl/, FreeRTOS/   upstream submodules
tests/             Catch2 tests
docs/              design notes
```

Contributor notes on architecture and conventions are in [CLAUDE.md](CLAUDE.md).

## License

CrossPad PC is free software under the **GNU General Public License v3.0 or
later** (`GPL-3.0-or-later`) — see [LICENSE](LICENSE).

Some parts carry their own, GPL-compatible terms:

- The code that came from LVGL's `lv_port_pc_vscode` template this project
  started from: MIT —
  [LICENSES/MIT-lv_port_pc_vscode.txt](LICENSES/MIT-lv_port_pc_vscode.txt)
- `lib/ml_synth/` (ML_SynthTools, Marcel Licence): GPL-3.0-or-later
- Submodules (LVGL, FreeRTOS, crosspad-core, crosspad-gui, the apps) and the
  libraries CMake fetches (RtMidi, RtAudio, ArduinoJson, SimpleBLE, Catch2):
  their own licenses. SimpleBLE is GPL-3.0, with a commercial license sold by
  its authors.
