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
CACHE_DIR = Path(__file__).parent / ".cache"


def _download_core() -> Path:
    """Download shared core from GitHub via gh CLI."""
    CACHE_DIR.mkdir(exist_ok=True)
    dest = CACHE_DIR / CORE_FILE

    # Check freshness (re-download every hour)
    if dest.exists():
        import time
        age = time.time() - dest.stat().st_mtime
        if age < 3600:
            return dest

    try:
        import base64
        result = subprocess.run(
            ["gh", "api", f"repos/{CORE_REPO}/contents/{CORE_FILE}",
             "--jq", ".content"],
            capture_output=True, text=True, check=True, timeout=15,
        )
        content = base64.b64decode(result.stdout.strip()).decode()
        dest.write_text(content)
        return dest
    except (subprocess.CalledProcessError, FileNotFoundError,
            subprocess.TimeoutExpired) as e:
        if dest.exists():
            print(f"Warning: Could not update core ({e}), using cached version.")
            return dest
        print(f"Error: Cannot download {CORE_FILE}: {e}")
        print("Make sure 'gh' CLI is installed and authenticated (gh auth login).")
        sys.exit(1)


def _load_core(path: Path):
    """Import the shared core module from downloaded file."""
    spec = importlib.util.spec_from_file_location("crosspad_app_manager", str(path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    # Find project root (parent of scripts/)
    project_dir = Path(__file__).resolve().parent.parent

    # Download and import shared core
    core_path = _download_core()
    core = _load_core(core_path)

    # PC platform config
    config = core.PlatformConfig(
        platform="pc",
        lib_dir="src/apps",
        official_org="CrossPad",
        lib_prefix="crosspad-",
    )

    # Delegate to shared CLI
    os.chdir(project_dir)
    core.cli_main(config)


if __name__ == "__main__":
    main()
