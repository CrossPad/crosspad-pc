# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

CrossPad PC — a desktop simulator for the CrossPad embedded device. Based on LVGL's lv_port_pc_vscode, extended with CrossPad-specific submodules. Runs LVGL GUI on desktop via SDL2 for rapid development and testing before deploying to ESP32-S3 hardware.

## Build

Windows with MSVC (Visual Studio 2022 Community):
```
build.bat
```
This calls vcvarsall.bat x64, then cmake+ninja with vcpkg toolchain. Output: `bin/main.exe`.

Manual build:
```bash
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

Run: `bin/main.exe`

**Dependencies:** SDL2 via vcpkg (`vcpkg install sdl2:x64-windows`), vcpkg at `C:\vcpkg`.

### CMake Options

| Option | Default | Description |
|---|---|---|
| `USE_FREERTOS` | OFF | Enable FreeRTOS multitasking mode |
| `LV_USE_DRAW_SDL` | OFF | SDL GPU-accelerated drawing |
| `LV_USE_LIBPNG` | OFF | PNG decoding |
| `LV_USE_LIBJPEG_TURBO` | OFF | JPEG decoding |
| `LV_USE_FFMPEG` | OFF | Video playback |
| `LV_USE_FREETYPE` | OFF | FreeType font rendering |
| `ASAN` | OFF | AddressSanitizer (Debug only) |

## Architecture

```
main.exe
  ├── src/main.c          — entry point, LVGL init + main loop
  ├── src/hal/hal.c       — SDL2 HAL: display, mouse, keyboard, mousewheel
  ├── lvgl/               — LVGL v9.x graphics library (submodule)
  ├── crosspad-gui/       — CrossPad UI: theme, widgets, components (submodule)
  ├── crosspad-core/      — CrossPad portable core: events, settings, pads, protocol (submodule)
  └── FreeRTOS/           — FreeRTOS kernel (submodule, optional)
```

### Entry Points

- **Standard mode** (`USE_FREERTOS=OFF`): `src/main.c` — single-threaded `lv_timer_handler()` loop
- **FreeRTOS mode** (`USE_FREERTOS=ON`): `src/freertos_main.c` — RTOS tasks with POSIX port

### HAL Layer (`src/hal/hal.c`)

`sdl_hal_init(width, height)` creates SDL window and registers input devices (mouse with cursor, mousewheel, keyboard) into LVGL's default focus group.

### Submodules

- **crosspad-core** (`CrossPad/crosspad-core`): Portable C++ library — app registry, event bus, settings (IKeyValueStore), pad/LED management, STM32 SysEx protocol, platform abstractions (IClock, IUART, etc.). Originally an ESP-IDF component.
- **crosspad-gui** (`CrossPad/crosspad-gui`): LVGL-based UI components — CrossPad theme, styles, custom widgets (keypad buttons, radial menu, VU meter, file explorer, DFU panel, status bar, toast/modal). Originally an ESP-IDF component depending on lvgl and crosspad-core.
- **lvgl**: LVGL v9.x graphics library
- **FreeRTOS**: FreeRTOS Kernel with POSIX port for desktop simulation

### Key Configuration Files

- `lv_conf.h` — LVGL feature toggles (color depth 32-bit, FS drivers, widgets, etc.)
- `config/FreeRTOSConfig.h` — FreeRTOS settings (512 MB heap for desktop simulation)
- `CMakeLists.txt` — GCC warning flags are wrapped in `if(NOT MSVC)` for Windows MSVC compatibility

## Important Notes

- crosspad-core and crosspad-gui use `idf_component_register()` in their CMakeLists.txt (ESP-IDF build system). To integrate them into this PC simulator, their build needs to be adapted or their sources included manually in the top-level CMakeLists.txt.
- Display resolution is set in `src/main.c`: `sdl_hal_init(320, 480)` — matches the CrossPad hardware display.
- To switch demos, change the function call in `src/main.c` (e.g., `lv_demo_widgets()`, `lv_example_file_explorer_1()`).
- `LV_USE_FS_WIN32` is enabled with driver letter `'C'` for Windows file system access.
