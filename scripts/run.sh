#!/bin/bash
# Smart run: build if needed, then launch simulator
set -e
cd "$(dirname "$0")/.."

BIN="bin/CrossPad"

needs_build() {
    # No binary at all
    [ ! -f "$BIN" ] && return 0
    # No build dir (never configured)
    [ ! -d "build" ] && return 0
    # Any source newer than binary
    find src lib/crosspad-core/src lib/crosspad-gui/src -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' 2>/dev/null \
        | while read f; do [ "$f" -nt "$BIN" ] && echo dirty && break; done | grep -q dirty
}

if needs_build; then
    echo "[CrossPad] Building..."
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build
    echo ""
fi

echo "[CrossPad] Launching simulator..."
exec "$BIN"
