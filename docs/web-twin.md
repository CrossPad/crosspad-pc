# The browser twin of a board

The CrossPad firmware — crosspad-core, crosspad-gui and every app, exactly as a
platform-idf checkout builds them for the ESP32-S3 — compiled to WebAssembly
over this repository's platform layer, running in a web page. Plugged into a
real board over Web MIDI it shows what the board's LCD shows, live, without the
screen ever travelling: the board sends what its LVGL *read*, and the twin
replays exactly that at the same LVGL times.

## Build

```bash
source ~/emsdk/emsdk_env.sh
cmake --preset web-twin          # the platform-idf checkout beside this one
cmake --build --preset web-twin  # -> web/build/CrossPad.{mjs,wasm,data}
```

`CROSSPAD_FIRMWARE_DIR` (the preset sets `../platform-idf`) is what makes it a
board's twin rather than this repository's simulator: crosspad-core,
crosspad-gui and the apps come from that checkout's `components/`, LVGL from its
`managed_components/lvgl__lvgl` (run one firmware build there first), the fonts
from its `main/fonts`, and its Settings and Firmware apps from `main/app/`
(their ESP half answered by `src/wasm/board/`). The apps are compiled with
`ESP_PLATFORM` as on the board. `lv_conf.h` takes the board's values where they
decide layout (`CROSSPAD_BOARD_FIRMWARE`). The same option also works for a
native build: this simulator, running a board's firmware.

Serve `web/` over http (any static server) and open:

| Page | What |
|---|---|
| `web/twin.html` | The live twin: the LCD of the CrossPad plugged into this computer. Chrome/Edge (Web MIDI with SysEx). `?log` prints the firmware's log. |
| `web/crosspad.html` | The same firmware as a standalone emulator, with the device body, pads and knob (no board needed). |

Other pages use `web/simtwin.js` (`startSim({ lcd: true })`) and
`web/boardlink.js` (`linkBoard(sim)`) the way `twin.html` does.

## How it stays the same screen

* **Inputs, not pixels.** The board's firmware (`platform-idf main/twin_mirror.cpp`)
  sends, while a page asks for it (companion notify `TWIN 1` every 2 s), SysEx
  `F0 7D 1F 09 <kind> <lv_tick> …`: every touch-panel and encoder read LVGL made
  while something was held or thrown, the pads as they enter the PadManager,
  the power button's resolved gesture, the USB host's MIDI as the firmware
  dispatched it, the status bar's telemetry, and a clock every 20 ms.
* **The board's time.** In the twin `lv_tick` is the board's tick, never past
  the last one the board vouched for; indevs are read only at the ticks the
  board read them (their read timers are paused), with that frame's data, after
  every timer due before it. `time()` is the board's wall clock.
* **The board's display.** `--lcd` (`startSim({ lcd: true })`) makes the
  display the 320×240 LCD alone with the GUI on the active screen, as on the
  board: LVGL's default theme, scroll timings and dropdown placement all follow
  the display's resolution. `crosspad.html` runs without it (the device body).
* **A safety net.** Every second the page asks `TWIN_STATE` and the twin
  compares it with its own (`platform-idf main/include/twin_state.h`, the same
  code on both sides: the object tree, focus, scroll, settings hash, telemetry);
  a difference that stays while the screen is at rest is corrected (app, focus,
  scroll, rebuild; settings over `TWIN_SETTINGS`). What only the board knows is
  fetched once: `TWIN_INFO` (Settings → Info), `TWIN_FW` (the Firmware app).

## The board's data

What the twin cannot get from inputs lives in `web/board/` (git-ignored; a copy
of *one* board). With the board on CDC:

```bash
python3 tools/twin/pull_assets.py      # its assets partition (icons) -> web/board/assets, then reconfigure
python3 tools/twin/pull_order.py       # the order its card lists directories in (kit list order)
python3 tools/twin/pull_sd.py && tools/twin/make_seed.sh   # a copy of its card -> web/board/sdseed.zip
```

They take `platform-idf/tools/fs_transfer.py` (from `CROSSPAD_FIRMWARE_DIR`, or
the checkout beside this one) and pyserial (ESP-IDF's Python has it).

## Test against the board

```bash
TWIN_URL=http://127.0.0.1:8931/crosspad-pc/web/twin.html?log \
TWIN_PY=~/.espressif/python_env/idf5.5_py3.12_env/bin/python \
node tools/twin/test/lock.cjs tools/twin/test/scen_full.cjs
```

Drives the board over CDC (`ENC_ROTATE`, `ENC_PRESS`, `PAD_PRESS`,
`PWR_GESTURE`, `TOUCH_INJECT` — the same read paths a hand takes), and after
every step compares the board's `SCREENSHOT` with the twin's LCD and the two
`TWIN_STATE`s. Needs Playwright (`PLAYWRIGHT=` its module path) and Chrome
(`CHROME=`). Scenarios never press in the Firmware app: that flips the board's
OTA preference.

## What it does not follow

* Audio levels (the Recorder's input meter, the Mixer's VU): they come from the
  board's audio, not from an input.
* A USB host's MIDI clock: this platform layer does not follow one.
* Overlays only the board's platform draws (OTA prompt, STM update, charging
  screen, the USB serial prompt).
* Files the board writes to its card after `web/board/` was pulled.
