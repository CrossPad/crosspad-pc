# Firmware kits of the simulator

One simulator binary per kit, as the board has one image per kit
(`platform-idf/config/profiles/`). `cmake -DCROSSPAD_FW_KIT=<kit>` picks the
file here; the default is `full`.

| Key | Meaning |
|---|---|
| `kit` | The kit id the binary carries (its descriptor, `CPFI-PC`), a-z then a-z 0-9 _, at most 15 characters |
| `display_name` | What the Firmware app shows for it |
| `version` | The kit's version; the simulator's own when absent |
| `apps` | Only these apps are in the launcher (directory names under `src/apps`). Every app is still compiled: the simulator's backend links against them |
| `exclude` | These apps are neither compiled nor registered |
| `standalone` | The app the firmware starts into, with no launcher behind it |
| `top_bar` | `false` hides the transport and status bar: the app gets the whole panel (default `true`) |
| `description` | The kit card's text, written into the package's manifest |

The binary is `bin/CrossPad` for `full` and `bin/CrossPad-<kit>` otherwise;
`scripts/make_fw_package.py` puts one on the virtual card as a firmware
package. `icons/<kit>.png` is the kit's icon, 64x64 as platform-idf's profile
icons (`scripts/fw_icon.py check`): it goes into the package as `icon.bin`
and is staged with the assets for a slot whose package is not on the card.
