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
    # Anything that feeds the build newer than the binary: sources and headers
    # of the simulator, both shared libraries (headers included) and every
    # app submodule under src/apps, plus the build description itself.
    [ -n "$(find src lib/crosspad-core lib/crosspad-gui cmake CMakeLists.txt .gitmodules lv_conf.h lv_conf.defaults \
        \( -name .git -o -name tests \) -prune -o \
        -type f \( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' -o -name '*.c' -o -name '*.cmake' \
            -o -name CMakeLists.txt -o -name .gitmodules -o -name 'lv_conf.*' -o -name '*.png' \) \
        -newer "$BIN" -print -quit 2>/dev/null)" ]
}

if needs_build; then
    echo "[CrossPad] Building..."
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
    cmake --build build
    echo ""
fi

echo "[CrossPad] Launching simulator..."
exec "$BIN"
