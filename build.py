"""CrossPad PC build script — calls vcvarsall + cmake + ninja."""

import subprocess
import sys
import os

PROJECT_DIR = os.path.dirname(os.path.abspath(__file__))
BUILD_DIR = os.path.join(PROJECT_DIR, "build")
VCVARSALL = r"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
VCPKG_TOOLCHAIN = "C:/vcpkg/scripts/buildsystems/vcpkg.cmake"

def get_msvc_env():
    """Run vcvarsall.bat and capture the resulting environment."""
    cmd = f'"{VCVARSALL}" x64 >nul 2>&1 && set'
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if result.returncode != 0:
        print("ERROR: vcvarsall.bat failed")
        sys.exit(1)
    env = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    return env

def run(cmd, env, **kwargs):
    print(f">>> {cmd}")
    r = subprocess.run(cmd, shell=True, env=env, cwd=PROJECT_DIR, **kwargs)
    if r.returncode != 0:
        print(f"FAILED (exit code {r.returncode})")
        sys.exit(r.returncode)

def main():
    clean = "--clean" in sys.argv

    print("[1/3] Setting up MSVC environment...")
    env = get_msvc_env()

    if clean and os.path.isdir(BUILD_DIR):
        import shutil, stat
        print("[    ] Cleaning build directory...")
        def force_remove(func, path, exc_info):
            os.chmod(path, stat.S_IWRITE)
            func(path)
        shutil.rmtree(BUILD_DIR, onexc=force_remove)

    print("[2/3] CMake configure...")
    run(
        f'cmake -B build -G Ninja'
        f' -DCMAKE_TOOLCHAIN_FILE={VCPKG_TOOLCHAIN}'
        f' -DCMAKE_BUILD_TYPE=Debug',
        env,
    )

    print("[3/3] CMake build...")
    run("cmake --build build", env)

    print("\nBuild OK -> bin/main.exe")

if __name__ == "__main__":
    main()
