#!/usr/bin/env python3
"""CrossPad PC App Manager — thin wrapper around shared core.

Downloads crosspad_app_manager.py from CrossPad/crosspad-apps if not cached,
then delegates all commands (list, install, remove, update, sync, tui).

Usage:
    python3 scripts/app_manager.py                  # Launch TUI
    python3 scripts/app_manager.py list              # List compatible apps
    python3 scripts/app_manager.py install mixer     # Install app
    python3 scripts/app_manager.py remove mixer      # Remove app
    python3 scripts/app_manager.py update --all      # Update all
    python3 scripts/app_manager.py sync              # Sync manifest
"""

import importlib.util
import os
import subprocess
import sys
from pathlib import Path

CORE_REPO = "CrossPad/crosspad-apps"
CORE_FILE = "crosspad_app_manager.py"
CORE_URL = f"https://raw.githubusercontent.com/{CORE_REPO}/main/{CORE_FILE}"
CORE_CACHE = Path(__file__).parent / ".cache" / CORE_FILE
CORE_STAMP = CORE_CACHE.with_name(".crosspad_app_manager.downloaded")
CORE_PREV = CORE_CACHE.with_name(CORE_FILE + ".prev")
CORE_TTL_S = 3600  # re-check upstream at most hourly


def _digest(path: Path) -> str:
    import hashlib
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _locally_modified() -> bool:
    """Is the cached copy someone's work in progress rather than our download?

    Without this the hourly refresh silently replaced a manager being edited
    in a sibling checkout — the TUI under test was upstream's, and every
    change appeared to do nothing.
    """
    if not CORE_CACHE.exists() or not CORE_STAMP.exists():
        return False
    try:
        return _digest(CORE_CACHE) != CORE_STAMP.read_text().strip()
    except OSError:
        return False


def _fetch_core() -> str | None:
    """Anonymous HTTPS first — reading a public file needs no GitHub account —
    then gh for machines where only gh gets through (a proxy set up for it)."""
    import urllib.request
    try:
        req = urllib.request.Request(CORE_URL, headers={"User-Agent": "crosspad-apps"})
        with urllib.request.urlopen(req, timeout=15) as r:
            return r.read().decode("utf-8")
    except (OSError, ValueError):
        pass
    try:
        import base64
        r = subprocess.run(
            ["gh", "api", f"repos/{CORE_REPO}/contents/{CORE_FILE}", "--jq", ".content"],
            capture_output=True, text=True, check=True, timeout=15)
        return base64.b64decode(r.stdout.strip()).decode()
    except (subprocess.CalledProcessError, FileNotFoundError,
            subprocess.TimeoutExpired, ValueError):
        return None


def _ensure_core():
    """Refresh the shared core from crosspad-apps, at most once an hour.

    A download replaces the cached copy only if it compiles, and the copy it
    replaces is kept as .prev — a broken or truncated upstream file must not
    leave a machine without a working manager. Offline, the cached copy runs.
    """
    if _locally_modified() and not os.environ.get("CROSSPAD_MANAGER_REFRESH"):
        print(f"Using your edited {CORE_FILE} "
              f"(CROSSPAD_MANAGER_REFRESH=1 to take upstream's again).")
        return
    if CORE_CACHE.exists():
        import time
        if time.time() - CORE_CACHE.stat().st_mtime < CORE_TTL_S:
            return
    text = _fetch_core()
    try:
        compile(text or "", CORE_FILE, "exec")
        usable = bool(text) and "class AppManager" in text
    except SyntaxError:
        usable = False
    if usable:
        CORE_CACHE.parent.mkdir(parents=True, exist_ok=True)
        if CORE_CACHE.exists():
            CORE_PREV.write_bytes(CORE_CACHE.read_bytes())
        CORE_CACHE.write_text(text, encoding="utf-8")
        CORE_STAMP.write_text(_digest(CORE_CACHE) + "\n")
        return
    if CORE_CACHE.exists():
        # The cached copy is the normal offline case; the screens say
        # "no connection" once, so this line stays out of their way.
        CORE_CACHE.touch()
        return
    print(f"No connection, and {CORE_FILE} has never been downloaded here.")
    print("  Connect to the internet once and start this again.")
    sys.exit(1)


def _load_core():
    _ensure_core()
    for path in (CORE_CACHE, CORE_PREV):
        if not path.exists():
            continue
        try:
            # An explicit loader: ".py.prev" has no suffix importlib recognises.
            from importlib.machinery import SourceFileLoader
            loader = SourceFileLoader("crosspad_app_manager", str(path))
            spec = importlib.util.spec_from_loader("crosspad_app_manager", loader)
            mod = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(mod)
        except Exception as e:  # noqa: BLE001 - fall back to the previous copy
            print(f"{path.name} failed to load ({type(e).__name__}: {e}); trying the previous copy.")
            continue
        if path == CORE_PREV:
            CORE_CACHE.write_bytes(CORE_PREV.read_bytes())
            CORE_STAMP.write_text(_digest(CORE_CACHE) + "\n")
        return mod
    print(f"No working {CORE_FILE}. Connect to the internet and start this again.")
    sys.exit(1)


# Loaded at import time (not just under __main__) so `from app_manager import
# AppManager` works for external callers — e.g. crosspad-mcp's
# `crosspad_apps_*` tools invoke this module programmatically via
# `python3 -c "from app_manager import AppManager; ..."`, not just as a CLI
# script. Matches the shape platform-idf/tools/app_manager.py already uses.
core = _load_core()

PC_CONFIG = core.PlatformConfig(
    platform="pc",
    lib_dir="src/apps",
    official_org="CrossPad",
    lib_prefix="crosspad-",
)


class AppManager(core.AppManager):
    """PC-specific AppManager with default config."""

    def __init__(self, project_dir: str):
        super().__init__(project_dir, PC_CONFIG)


def main():
    # Find project root (parent of scripts/)
    project_dir = Path(__file__).resolve().parent.parent

    # Delegate to shared CLI
    os.chdir(project_dir)
    core.cli_main(PC_CONFIG)


if __name__ == "__main__":
    main()
