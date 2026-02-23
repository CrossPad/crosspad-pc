#!/usr/bin/env bash
# submodule-mode.sh — Restore proper git submodules (removes Windows junctions).
# Without arguments: restores to commits saved by dev-mode.sh.
# With --latest: checks out current HEAD of each submodule's tracked branch and pins that.
#
# Usage:
#   ./submodule-mode.sh              # restore to pre-dev-mode pins
#   ./submodule-mode.sh --latest     # update to latest and pin
set -e

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
PINS_FILE="$REPO_ROOT/.dev-mode-pins"

remove_junction() {
    local rel_path="$1"
    if [ -d "$REPO_ROOT/$rel_path" ]; then
        local win_path tmpbat
        win_path="$(cygpath -w "$REPO_ROOT/$rel_path")"
        tmpbat="$(cygpath -w "$REPO_ROOT/_rmjunction_tmp.bat")"
        printf '@echo off\r\nrmdir "%s"\r\n' "$win_path" > "$REPO_ROOT/_rmjunction_tmp.bat"
        cmd.exe //c "$tmpbat" 2>/dev/null || rm -rf "$REPO_ROOT/$rel_path"
        rm -f "$REPO_ROOT/_rmjunction_tmp.bat"
        echo "   removed $rel_path"
    fi
}

echo "==> Removing junctions..."
remove_junction "crosspad-core"
remove_junction "crosspad-gui"

echo "==> Restoring submodules..."
cd "$REPO_ROOT"
git -c protocol.file.allow=always submodule update --init crosspad-core
git -c protocol.file.allow=always submodule update --init crosspad-gui

if [ "$1" = "--latest" ]; then
    echo "==> Checking out latest from tracked branch..."
    cd crosspad-core && git fetch origin && git checkout origin/main && cd "$REPO_ROOT"
    cd crosspad-gui  && git fetch origin && git checkout origin/master && cd "$REPO_ROOT"
    git add crosspad-core crosspad-gui
    echo ""
    echo "Submodules updated to latest. Don't forget to commit the updated pins:"
    echo "   git commit -m 'chore: update crosspad-core and crosspad-gui to latest'"
elif [ -f "$PINS_FILE" ]; then
    CORE_PIN=$(grep "crosspad-core=" "$PINS_FILE" | cut -d= -f2)
    GUI_PIN=$(grep "crosspad-gui=" "$PINS_FILE"  | cut -d= -f2)
    echo "==> Restoring to saved pins..."
    echo "   crosspad-core: $CORE_PIN"
    echo "   crosspad-gui:  $GUI_PIN"
    cd crosspad-core && git checkout "$CORE_PIN" && cd "$REPO_ROOT"
    cd crosspad-gui  && git checkout "$GUI_PIN"  && cd "$REPO_ROOT"
else
    echo "No .dev-mode-pins file found — submodules at index HEAD."
fi

echo ""
echo "Submodule mode active."
