#!/usr/bin/env bash
# dev-mode.sh — Replace submodules with Windows junctions to local repos.
# Junctions are transparent to CMake — changes are instant, no fetch/checkout needed.
# Saves current pinned commits to .dev-mode-pins so submodule-mode.sh can restore them.
#
# Usage: ./dev-mode.sh
set -e

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
CORE_LOCAL="/c/Users/Mateusz/GIT/crosspad-core"
GUI_LOCAL="/c/Users/Mateusz/GIT/crosspad-gui"
PINS_FILE="$REPO_ROOT/.dev-mode-pins"

make_junction() {
    local link_unix="$1"
    local target_unix="$2"
    local link_win target_win tmpbat
    link_win="$(cygpath -w "$link_unix")"
    target_win="$(cygpath -w "$target_unix")"
    tmpbat="$(cygpath -w "$REPO_ROOT/_mkjunction_tmp.bat")"
    printf '@echo off\r\nmklink /J "%s" "%s"\r\n' "$link_win" "$target_win" > "$REPO_ROOT/_mkjunction_tmp.bat"
    cmd.exe //c "$tmpbat"
    rm -f "$REPO_ROOT/_mkjunction_tmp.bat"
}

check_clean_submodule() {
    local path="$1"
    if [ -d "$REPO_ROOT/$path/.git" ] || [ -f "$REPO_ROOT/$path/.git" ]; then
        local status
        status=$(cd "$REPO_ROOT/$path" && git status --porcelain 2>/dev/null)
        if [ -n "$status" ]; then
            echo "WARNING: $path has uncommitted changes — commit or stash them first."
            exit 1
        fi
    fi
}

echo "==> Checking for uncommitted changes in submodules..."
check_clean_submodule "crosspad-core"
check_clean_submodule "crosspad-gui"

echo "==> Saving current pinned commits..."
cd "$REPO_ROOT"
CORE_PIN=$(git submodule status crosspad-core | awk '{print $1}' | tr -d '+-')
GUI_PIN=$(git submodule status crosspad-gui | awk '{print $1}' | tr -d '+-')
echo "crosspad-core=$CORE_PIN" > "$PINS_FILE"
echo "crosspad-gui=$GUI_PIN" >> "$PINS_FILE"
echo "   crosspad-core: $CORE_PIN"
echo "   crosspad-gui:  $GUI_PIN"

echo "==> Deinitializing submodules..."
git submodule deinit -f crosspad-core
git submodule deinit -f crosspad-gui
rm -rf .git/modules/crosspad-core
rm -rf .git/modules/crosspad-gui
rm -rf crosspad-core
rm -rf crosspad-gui

echo "==> Creating Windows junctions..."
make_junction "$REPO_ROOT/crosspad-core" "$CORE_LOCAL"
make_junction "$REPO_ROOT/crosspad-gui" "$GUI_LOCAL"

if [ -d "crosspad-core/src" ] && [ -d "crosspad-gui/src" ]; then
    echo ""
    echo "Dev mode active. Junctions created:"
    echo "   crosspad-core -> $CORE_LOCAL"
    echo "   crosspad-gui  -> $GUI_LOCAL"
    echo ""
    echo "Changes in crosspad-core and crosspad-gui are now instantly visible."
    echo "Run ./submodule-mode.sh to restore pinned submodules."
else
    echo "ERROR: Junction creation failed."
    exit 1
fi
