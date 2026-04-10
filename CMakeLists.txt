cmake_minimum_required(VERSION 3.14)
project(crosspad_pc VERSION 0.3.1 LANGUAGES C CXX)

# Set the correct FreeRTOS port for the current compiler
if(MSVC OR MINGW)
    set(FREERTOS_PORT MSVC_MINGW CACHE STRING "FreeRTOS port" FORCE)
else()
    set(FREERTOS_PORT GCC_POSIX CACHE STRING "FreeRTOS port" FORCE)
endif()

set(LVGL_PRO_PROJECT_DIR
    ""
    CACHE PATH "Path to LVGL Pro Project folder")

option(USE_MIDI "Enable MIDI input/output via RtMidi" ON)
option(USE_AUDIO "Enable system audio output via RtAudio" ON)
option(USE_BLE "Enable BLE MIDI via SimpleBLE" ON)

# FreeRTOS — always enabled (required for crosspad-core event bus & LVGL OSAL)
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/config)
target_compile_definitions(freertos_config INTERFACE projCOVERAGE_TEST=0)

add_subdirectory(FreeRTOS)

include_directories(${PROJECT_SOURCE_DIR}/FreeRTOS/include)
include_directories(${PROJECT_SOURCE_DIR}/config)

if(MSVC OR MINGW)
    set(FREERTOS_PORT_DIR ${PROJECT_SOURCE_DIR}/FreeRTOS/portable/MSVC-MingW)
else()
    set(FREERTOS_PORT_DIR ${PROJECT_SOURCE_DIR}/FreeRTOS/portable/ThirdParty/GCC/Posix)
endif()
include_directories(${FREERTOS_PORT_DIR})

file(GLOB FREERTOS_SOURCES
    "${PROJECT_SOURCE_DIR}/FreeRTOS/*.c"
    "${PROJECT_SOURCE_DIR}/FreeRTOS/portable/MemMang/heap_4.c"
    "${FREERTOS_PORT_DIR}/*.c"
)

# Main include files of the project
include_directories(${PROJECT_SOURCE_DIR}/main/inc)

# Define options for LVGL with default values (OFF)
option(LV_USE_DRAW_SDL "Use SDL draw unit" OFF)
option(LV_USE_LIBPNG "Use libpng to decode PNG" OFF)
option(LV_USE_LIBJPEG_TURBO "Use libjpeg turbo to decode JPEG" OFF)
option(LV_USE_FFMPEG "Use libffmpeg to display video using lv_ffmpeg" OFF)
option(LV_USE_FREETYPE "Use freetype library" OFF)

# Set C and C++ standards
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set the output path for the executable
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
set(WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})

# Find and include SDL2 library
find_package(SDL2 REQUIRED)

include(FetchContent)

# Suppress warnings from all third-party code (LVGL, RtMidi, RtAudio, etc.)
if(MSVC)
    set(_SAVED_C_FLAGS "${CMAKE_C_FLAGS}")
    set(_SAVED_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
    string(REGEX REPLACE "/W[0-4]" "/w" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
    string(REGEX REPLACE "/W[0-4]" "/w" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

# ArduinoJson (header-only, required by crosspad-core settings)
FetchContent_Declare(
    ArduinoJson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG        v7.3.0
)
FetchContent_MakeAvailable(ArduinoJson)

# MIDI support via RtMidi
if(USE_MIDI)
    FetchContent_Declare(
        rtmidi
        GIT_REPOSITORY https://github.com/thestk/rtmidi.git
        GIT_TAG        6.0.0
    )
    set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(RTMIDI_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(rtmidi)
    message(STATUS "MIDI support enabled (RtMidi)")
endif()

# Audio support via RtAudio
if(USE_AUDIO)
    FetchContent_Declare(
        rtaudio
        GIT_REPOSITORY https://github.com/thestk/rtaudio.git
        GIT_TAG        6.0.0
    )
    set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    # Rename RtAudio's "uninstall" target to avoid collision with RtMidi's
    set(RTAUDIO_TARGETNAME_UNINSTALL "rtaudio-uninstall" CACHE STRING "" FORCE)
    # Keep dynamic CRT (/MDd) — don't let RtAudio switch to static /MT
    set(RTAUDIO_STATIC_MSVCRT OFF)
    FetchContent_MakeAvailable(rtaudio)
    message(STATUS "Audio support enabled (RtAudio)")
endif()

# BLE MIDI support via SimpleBLE
if(USE_BLE)
    FetchContent_Declare(
        simpleble
        GIT_REPOSITORY https://github.com/OpenBluetoothToolbox/SimpleBLE.git
        GIT_TAG        v0.7.3
        SOURCE_SUBDIR  simpleble
    )
    FetchContent_MakeAvailable(simpleble)
    # SimpleBLE sets /WX /W1 internally — override to suppress warnings
    if(MSVC)
        target_compile_options(simpleble PRIVATE /w /WX-)
    endif()
    message(STATUS "BLE support enabled (SimpleBLE)")
endif()

# Add compile definitions based on the selected options
add_compile_definitions($<$<BOOL:${LV_USE_DRAW_SDL}>:LV_USE_DRAW_SDL=1>)
add_compile_definitions($<$<BOOL:${LV_USE_LIBPNG}>:LV_USE_LIBPNG=1>)
add_compile_definitions($<$<BOOL:${LV_USE_LIBJPEG_TURBO}>:LV_USE_LIBJPEG_TURBO=1>)
add_compile_definitions($<$<BOOL:${LV_USE_FFMPEG}>:LV_USE_FFMPEG=1>)

# Force LVGL to build as static library (vcpkg sets BUILD_SHARED_LIBS=ON
# which breaks ThorVG linking — it can't resolve lv_realloc/lv_strdup/lv_zalloc
# when built as a separate DLL)
set(BUILD_SHARED_LIBS OFF)

add_subdirectory(lvgl)
target_include_directories(lvgl PUBLIC ${PROJECT_SOURCE_DIR} ${SDL2_INCLUDE_DIRS})

# Restore warning level for project code
if(MSVC)
    set(CMAKE_C_FLAGS "${_SAVED_C_FLAGS}")
    set(CMAKE_CXX_FLAGS "${_SAVED_CXX_FLAGS}")
endif()

# ── crosspad-core portable sources (auto-discover, excluding platform-specific) ──
file(GLOB_RECURSE CROSSPAD_CORE_SOURCES lib/crosspad-core/src/*.cpp)
list(FILTER CROSSPAD_CORE_SOURCES EXCLUDE REGEX "EspEventBus\\.cpp$")
list(FILTER CROSSPAD_CORE_SOURCES EXCLUDE REGEX "SyncEventBus\\.cpp$")
list(FILTER CROSSPAD_CORE_SOURCES EXCLUDE REGEX "Stm32Manager\\.cpp$")
# FreeRtosEventBus.cpp stays — used as the event bus implementation

# ── crosspad-gui sources (auto-discover) ──
file(GLOB_RECURSE CROSSPAD_GUI_SOURCES lib/crosspad-gui/src/*.cpp)

# ── Auto-reconfigure when source files are added/removed ──
# Ninja watches these directories and triggers cmake reconfigure on changes
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/lib/crosspad-core/src"
    "${CMAKE_SOURCE_DIR}/lib/crosspad-gui/src"
    "${CMAKE_SOURCE_DIR}/src/apps"
    "${CMAKE_SOURCE_DIR}/src/pc_stubs"
    "${CMAKE_SOURCE_DIR}/src/updater"
)

# ── PC platform stubs (event bus, singletons, interface impls) ──
set(PC_STUB_SOURCES
    src/pc_stubs/PcPlatformStubs.cpp
    src/pc_stubs/PcApp.cpp
    src/pc_stubs/PcHttpClient.cpp
)

# Windows application icon
if(WIN32)
    set(WIN_RESOURCES crosspad.rc)
else()
    set(WIN_RESOURCES "")
endif()

set(MAIN_SOURCES ${WIN_RESOURCES} src/mouse_cursor_icon.c src/hal/hal.c
    src/stm32_emu/Stm32EmuWindow.cpp
    src/stm32_emu/EmuJackPanel.cpp
    src/stm32_emu/EmuSdCardSlot.cpp
    src/stm32_emu/KeyboardCapture.cpp
    src/uart/PcUart.cpp
    src/crosspad_app.cpp
    src/remote/RemoteControl.cpp
    ${CROSSPAD_CORE_SOURCES}
    ${CROSSPAD_GUI_SOURCES}
    ${PC_STUB_SOURCES}
)
set(MAIN_LIBS lvgl lvgl::examples lvgl::demos lvgl::thorvg ${SDL2_LIBRARIES} ArduinoJson)

# FreeRTOS entry point + platform port
if(MSVC OR MINGW)
    list(APPEND MAIN_SOURCES src/freertos_main.cpp src/freertos/freertos_win32_port.c ${FREERTOS_SOURCES})
else()
    list(APPEND MAIN_SOURCES src/freertos_main.cpp src/freertos/freertos_posix_port.c ${FREERTOS_SOURCES})
endif()
list(APPEND MAIN_LIBS freertos_config freertos_kernel)


if(NOT MSVC)
    list(APPEND MAIN_LIBS m pthread)
endif()

# MIDI sources and libraries
if(USE_MIDI)
    list(APPEND MAIN_SOURCES src/midi/PcMidi.cpp)
    list(APPEND MAIN_LIBS rtmidi)
    if(WIN32)
        list(APPEND MAIN_LIBS winmm)
    endif()
endif()

# BLE MIDI sources and libraries
if(USE_BLE)
    list(APPEND MAIN_SOURCES src/midi/PcBleMidi.cpp)
    list(APPEND MAIN_LIBS simpleble::simpleble)
endif()

# Audio sources and libraries
if(USE_AUDIO)
    list(APPEND MAIN_SOURCES src/audio/PcAudio.cpp src/audio/PcAudioInput.cpp src/audio/PcAudioModule.cpp)
    list(APPEND MAIN_LIBS rtaudio)

    # ML_SynthTools vendored FM synth engine
    set(ML_SYNTH_SOURCES
        lib/ml_synth/ml_fm.cpp
        lib/ml_synth/ml_status_stub.cpp
        lib/ml_synth/ml_utils_stub.cpp
    )

    # Synth engine wrapper (platform-level, like ESP32's audio codec)
    set(ML_PIANO_SYNTH_SOURCES src/synth/MlPianoSynth.cpp)

    list(APPEND MAIN_SOURCES ${ML_SYNTH_SOURCES} ${ML_PIANO_SYNTH_SOURCES})
endif()

# ── Built-in apps (always present) ──────────────────────────────────────
foreach(_builtin settings citest update)
    if(EXISTS "${CMAKE_SOURCE_DIR}/src/apps/${_builtin}/CMakeLists.txt")
        add_subdirectory(src/apps/${_builtin})
    endif()
endforeach()
list(APPEND MAIN_SOURCES ${SETTINGS_APP_SOURCES} ${CITEST_APP_SOURCES} ${UPDATE_APP_SOURCES})

# ── Installable apps (submodules in src/apps/crosspad-*) ────────────────
file(GLOB _app_submodule_dirs "${CMAKE_SOURCE_DIR}/src/apps/crosspad-*/")
foreach(_app_dir ${_app_submodule_dirs})
    if(NOT IS_DIRECTORY "${_app_dir}")
        continue()
    endif()
    get_filename_component(_app_name "${_app_dir}" NAME)

    # App Store is dev-only — skip when BUILD_TESTING is OFF (release builds)
    if(NOT BUILD_TESTING AND _app_name STREQUAL "crosspad-appstore")
        message(STATUS "Skipping dev-only app: ${_app_name}")
        continue()
    endif()

    # Collect sources from src/
    file(GLOB _app_sources "${_app_dir}/src/*.cpp")
    if(_app_sources)
        list(APPEND MAIN_SOURCES ${_app_sources})
    endif()

    message(STATUS "App submodule: ${_app_name} (${_app_sources})")
endforeach()

# Updater module (version check, download, self-update)
list(APPEND MAIN_SOURCES src/updater/PcUpdater.cpp)

# Version header — always generated into build/generated/ from PROJECT_VERSION.
# CI can override via -DCROSSPAD_VERSION_OVERRIDE=x.y.z from git tag.
if(DEFINED CROSSPAD_VERSION_OVERRIDE)
    set(CP_VERSION "${CROSSPAD_VERSION_OVERRIDE}")
else()
    set(CP_VERSION "${PROJECT_VERSION}")
endif()
message(STATUS "CrossPad version: ${CP_VERSION}")
configure_file(cmake/version.h.in "${CMAKE_BINARY_DIR}/generated/crosspad_pc_version.h" @ONLY)


# Auto-generate app registry initialization
include(cmake/generate_registry.cmake)
set(APP_REGISTRY_FILE "${CMAKE_BINARY_DIR}/app_registry_init.cpp")
set(_registry_excludes "")
if(NOT BUILD_TESTING)
    list(APPEND _registry_excludes "crosspad-appstore")
endif()
generate_app_registry("${APP_REGISTRY_FILE}" "${PROJECT_SOURCE_DIR}/src/apps" ${_registry_excludes})
list(APPEND MAIN_SOURCES "${APP_REGISTRY_FILE}")

# Custom target to run the executable
add_custom_target(run COMMAND ${EXECUTABLE_OUTPUT_PATH}/CrossPad DEPENDS CrossPad)

# Conditionally include and link SDL2_image if LV_USE_DRAW_SDL is enabled
if(LV_USE_DRAW_SDL)
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")
    find_package(SDL2_image REQUIRED)
    target_include_directories(lvgl PUBLIC ${SDL2_IMAGE_INCLUDE_DIRS})
    list(APPEND MAIN_LIBS ${SDL2_IMAGE_LIBRARIES})
endif()

# Conditionally include and link libpng if LV_USE_LIBPNG is enabled
if(LV_USE_LIBPNG)
    find_package(PNG REQUIRED)
    target_include_directories(lvgl PUBLIC ${PNG_INCLUDE_DIRS})
    list(APPEND MAIN_LIBS ${PNG_LIBRARIES})
endif()

# Conditionally include and link libjpeg-turbo if LV_USE_LIBJPEG_TURBO is enabled
if(LV_USE_LIBJPEG_TURBO)
    find_package(JPEG REQUIRED)
    target_include_directories(lvgl PUBLIC ${JPEG_INCLUDE_DIRS})
    list(APPEND MAIN_LIBS ${JPEG_LIBRARIES})
endif()

# Conditionally include and link FFmpeg libraries if LV_USE_FFMPEG is enabled
if(LV_USE_FFMPEG)
    list(APPEND MAIN_LIBS avformat avcodec avutil swscale z)
endif()

# Conditionally include and link FreeType if LV_USE_FREETYPE is enabled
if(LV_USE_FREETYPE)
    find_package(Freetype REQUIRED)
    target_include_directories(lvgl PUBLIC ${FREETYPE_INCLUDE_DIRS})
    list(APPEND MAIN_LIBS ${FREETYPE_LIBRARIES})
endif()

if(LVGL_PRO_PROJECT_DIR)
  if(EXISTS "${LVGL_PRO_PROJECT_DIR}/CMakeLists.txt")
    add_subdirectory(${LVGL_PRO_PROJECT_DIR} ui)
    list(APPEND MAIN_LIBS lib-ui)

    target_compile_definitions(lib-ui PUBLIC LV_LVGL_H_INCLUDE_SIMPLE)
    target_include_directories(lib-ui PUBLIC
      ${LVGL_PRO_PROJECT_DIR}  # local headers like ui.h, generated files, etc.
      ${CMAKE_SOURCE_DIR}/lvgl # include "lvgl.h" works
    )
    message(STATUS "LVGL Pro Project enabled from: ${LVGL_PRO_PROJECT_DIR}")
  else()
    message(
      WARNING
        "LVGL_PRO_PROJECT_DIR specified but no CMakeLists.txt found at: ${LVGL_PRO_PROJECT_DIR}"
    )
  endif()
endif()

add_executable(CrossPad ${MAIN_SOURCES})
target_compile_definitions(CrossPad PRIVATE
    LV_CONF_INCLUDE_SIMPLE
    USE_LVGL=1
    CP_LCD_HOR_RES=320
    CP_LCD_VER_RES=240
    PLATFORM_PC=1
    CROSSPAD_REGISTER_APP_NAMED_FUNCTIONS=1
)
target_include_directories(CrossPad PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/lib/crosspad-core/include
    ${PROJECT_SOURCE_DIR}/lib/crosspad-gui/include
    ${PROJECT_SOURCE_DIR}/lib/ml_synth
    ${PROJECT_SOURCE_DIR}/lib
    ${PROJECT_SOURCE_DIR}/lvgl/src
    ${CMAKE_BINARY_DIR}/generated
)
# Add include dirs from installable app submodules
foreach(_app_dir ${_app_submodule_dirs})
    if(IS_DIRECTORY "${_app_dir}/include")
        target_include_directories(CrossPad PRIVATE "${_app_dir}/include")
        # Also add the nested dir so apps can use short includes (e.g. "MixerApp.hpp")
        get_filename_component(_app_name "${_app_dir}" NAME)
        if(IS_DIRECTORY "${_app_dir}/include/${_app_name}")
            target_include_directories(CrossPad PRIVATE "${_app_dir}/include/${_app_name}")
        endif()
    endif()
endforeach()
target_link_libraries(CrossPad ${MAIN_LIBS})

# Winsock + COM (folder picker dialog) for Windows
if(WIN32)
    target_link_libraries(CrossPad ws2_32 ole32 shell32 winhttp)

    # Copy SDL2.dll next to the exe so dev builds match release layout.
    # This ensures the auto-updater can swap exe+dll without PATH issues.
    find_file(SDL2_DLL_PATH SDL2.dll
        PATHS "${SDL2_DIR}" "${_VCPKG_INSTALLED_DIR}/x64-windows/bin"
              "${VCPKG_INSTALLED_DIR}/x64-windows/bin"
              "C:/vcpkg/installed/x64-windows/bin"
        NO_DEFAULT_PATH
    )
    if(SDL2_DLL_PATH)
        add_custom_command(TARGET CrossPad POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${SDL2_DLL_PATH}"
                "$<TARGET_FILE_DIR:CrossPad>/SDL2.dll"
            COMMENT "Copying SDL2.dll to output directory"
        )
    endif()
endif()

target_compile_definitions(CrossPad PRIVATE USE_FREERTOS=1)

# Dev build flag — exposed to C++ so apps can show dev-only info
if(BUILD_TESTING)
    target_compile_definitions(CrossPad PRIVATE CROSSPAD_DEV_BUILD=1)
endif()


# MIDI compile definition
if(USE_MIDI)
    target_compile_definitions(CrossPad PRIVATE USE_MIDI=1)
endif()

if(USE_BLE)
    target_compile_definitions(CrossPad PRIVATE USE_BLE=1)
endif()

if(USE_AUDIO)
    target_compile_definitions(CrossPad PRIVATE USE_AUDIO=1)
    # Suppress warnings for vendored ML_SynthTools code
    if(MSVC)
        set_source_files_properties(
            lib/ml_synth/ml_fm.cpp
            lib/ml_synth/ml_status_stub.cpp
            lib/ml_synth/ml_utils_stub.cpp
            PROPERTIES COMPILE_FLAGS "/w"
        )
    else()
        set_source_files_properties(
            lib/ml_synth/ml_fm.cpp
            lib/ml_synth/ml_status_stub.cpp
            lib/ml_synth/ml_utils_stub.cpp
            PROPERTIES COMPILE_FLAGS "-w"
        )
    endif()
endif()

# On Windows, GUI applications do not show a console by default,
# which hides log output. This ensures a console is available for logging.
if (WIN32)
    if (MSVC)
        target_link_options(CrossPad PRIVATE "/SUBSYSTEM:CONSOLE")
    else()
        target_link_options(CrossPad PRIVATE "-mconsole")
    endif()
endif()

# ── Tests ──
option(BUILD_TESTING "Build Catch2 unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()

# Apply additional compile options if the build type is Debug
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(STATUS "Debug mode enabled")

    if(NOT MSVC)
        target_compile_options(lvgl PRIVATE
            -pedantic-errors
            -Wall
            -Wclobbered
            -Wdeprecated
            -Wdouble-promotion
            -Wempty-body
            -Wextra
            -Wformat-security
            -Wmaybe-uninitialized
            # -Wmissing-prototypes
            -Wpointer-arith
            -Wmultichar
            -Wno-pedantic # ignored for now, we convert functions to pointers for properties table.
            -Wreturn-type
            -Wshadow
            -Wshift-negative-value
            -Wsizeof-pointer-memaccess
            -Wtype-limits
            -Wundef
            -Wuninitialized
            -Wunreachable-code
            -Wfloat-conversion
            -Wstrict-aliasing
        )
    endif()

if (ASAN)
    message(STATUS "AddressSanitizer enabled")

    # Add AddressSanitizer flags
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address")
else()
    message(STATUS "AddressSanitizer disabled")
endif()
endif()
