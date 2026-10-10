#!/usr/bin/env python3
"""Generate .vscode/settings.json from .vscode/settings.template.json.

Idempotent — does nothing if settings.json already exists. Run with --force
to regenerate. Designed to run automatically on the first cmake configure
so new users get task buttons + recommended VS Code config without manual
setup.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
TEMPLATE = REPO_ROOT / ".vscode" / "settings.template.json"
OUT = REPO_ROOT / ".vscode" / "settings.json"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--force", action="store_true",
                    help="overwrite existing settings.json")
    args = ap.parse_args()

    if not TEMPLATE.is_file():
        print(f"[setup_vscode] template missing: {TEMPLATE}", file=sys.stderr)
        return 1

    if OUT.exists() and not args.force:
        return 0

    OUT.write_text(TEMPLATE.read_text(encoding="utf-8"), encoding="utf-8")
    print(f"[setup_vscode] wrote {OUT.relative_to(REPO_ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
