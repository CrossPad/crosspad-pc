# CrossPad PC

Desktop simulator for the [CrossPad](https://github.com/CrossPad) 16-pad MIDI controller. It runs the
same crosspad-core logic, crosspad-gui (LVGL 9) interface and apps as the ESP32-S3 firmware, in an SDL2
window that draws the device: 320×240 LCD, 4×4 pads, encoder and power button. Linux and Windows are the
main targets; it runs on macOS too, but USB serial (CDC), MIDI and audio are not well tested there.

## Quickstart (Ubuntu / Debian)

```bash
sudo apt-get install -y build-essential cmake ninja-build git pkg-config libsdl2-dev libasound2-dev libpulse-dev libdbus-1-dev
git clone --recursive https://github.com/CrossPad/crosspad-pc.git ~/crosspad-pc
cd ~/crosspad-pc
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build
./bin/CrossPad
```

## Documentation

- [PC simulator](https://crosspad.app/docs/start-here/pc-simulator/): build and run on Linux, macOS and Windows (Visual Studio 2022 + vcpkg), and run the tests
- [Troubleshooting → Simulator](https://crosspad.app/docs/troubleshooting/#simulator) · [Platform parity](https://crosspad.app/docs/concepts/platform-parity/) · [Write your first app](https://crosspad.app/docs/how-to/new-app/)
- [CLAUDE.md](CLAUDE.md): contributor notes (CMake options, architecture, app manager, remote-control port)

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). Code from LVGL's `lv_port_pc_vscode` template is MIT
([LICENSES/MIT-lv_port_pc_vscode.txt](LICENSES/MIT-lv_port_pc_vscode.txt)); `lib/ml_synth/`
(ML_SynthTools, Marcel Licence) is GPL-3.0-or-later. Submodules (LVGL, FreeRTOS, crosspad-core,
crosspad-gui, the apps) and the libraries CMake fetches (RtMidi, RtAudio, ArduinoJson, SimpleBLE, fmt,
Catch2) keep their own licenses; SimpleBLE is GPL-3.0, with a commercial license sold by its authors.
