#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Put a simulator binary on the virtual SD card as a firmware package.

The simulator's counterpart of platform-idf's tools/make_fw_package.py: the
package is crosspad/firmware/<kit>-<version>/ on the card, holding the binary
as CrossPad.pc, the kit's icon as icon.bin, a manifest.json and .complete.
Kit and version come from the binary's own descriptor (src/fw/PcFwDesc.cpp),
as a board package's come from its CPFI block; the kit card's text, apps and
icon from config/fw_kits/<kit>.json and config/fw_kits/icons/<kit>.png.

    python3 scripts/make_fw_package.py bin/CrossPad-doom
    python3 scripts/make_fw_package.py bin/CrossPad-doom --card ~/.crosspad/sdcard

The card defaults to the simulator's mounted one (sdcard_path in the
profile's device_preferences.json; CROSSPAD_PROFILE_DIR names the profile).
"""

import argparse
import hashlib
import json
import os
import shutil
import sys
from pathlib import Path

import fw_icon

REPO = Path(__file__).resolve().parent.parent
KITS = REPO / "config" / "fw_kits"
MARKER = b"CPFI-PC1;"
IMAGE_NAME = "CrossPad.pc"
ICON_NAME = "icon.bin"
DESC_MAX = 256


def read_desc(exe: Path) -> dict:
    data = exe.read_bytes()
    at = data.find(MARKER)
    if at < 0:
        sys.exit(f"{exe}: no firmware descriptor -- not a simulator binary built with src/fw")
    end = data.find(b"\n", at, at + DESC_MAX)
    body = data[at + len(MARKER):end if end >= 0 else at + DESC_MAX].decode("utf-8", "replace")
    return dict(f.split("=", 1) for f in body.split(";") if "=" in f)


def default_card() -> Path:
    profile = Path(os.environ.get("CROSSPAD_PROFILE_DIR") or Path.home() / ".crosspad")
    prefs = profile / "device_preferences.json"
    try:
        card = json.loads(prefs.read_text()).get("sdcard_path", "")
    except (OSError, ValueError):
        card = ""
    if not card:
        sys.exit(f"no card mounted in {prefs}; pass --card")
    return Path(card)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe", type=Path, help="the simulator binary (bin/CrossPad-<kit>)")
    ap.add_argument("--card", type=Path, help="the virtual card's root (default: the simulator's mounted card)")
    args = ap.parse_args()

    desc = read_desc(args.exe)
    if desc.get("project") != "crosspad-pc":
        sys.exit(f"{args.exe}: descriptor names project {desc.get('project')!r}, not crosspad-pc")
    kit, version = desc["kit"], desc["version"]
    card = args.card or default_card()
    pkg = card / "crosspad" / "firmware" / f"{kit}-{version}"
    if pkg.exists():
        shutil.rmtree(pkg)
    pkg.mkdir(parents=True)

    image = pkg / IMAGE_NAME
    shutil.copy2(args.exe, image)
    image.chmod(0o755)
    files = [image]
    icon_png = KITS / "icons" / f"{kit}.png"
    if icon_png.is_file():
        (pkg / ICON_NAME).write_bytes(fw_icon.convert(icon_png.read_bytes()))
        files.append(pkg / ICON_NAME)
    kit_json = KITS / f"{kit}.json"
    kit_def = json.loads(kit_json.read_text()) if kit_json.is_file() else {}
    manifest = {
        "name": pkg.name,
        "kit": kit,
        "version": version,
        "display_name": desc.get("name", kit),
        "description": kit_def.get("description", ""),
        "apps": [a.removeprefix("crosspad-") for a in kit_def.get("apps", [])],
        "board_rev": 0,
        "files": [{"name": f.name, "size": f.stat().st_size, "sha256": hashlib.sha256(f.read_bytes()).hexdigest()}
                  for f in files],
    }
    (pkg / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (pkg / ".complete").write_text("")
    print(f"{pkg}: {kit} {version}, {image.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
