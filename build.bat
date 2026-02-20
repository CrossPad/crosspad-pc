@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64
pushd "%~dp0"
if exist build rmdir /s /q build
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Debug -DUSE_FREERTOS=ON
cmake --build build
popd
endlocal

@REM Works for me on Windows without WSL, might need some adjustments for your setup. 
