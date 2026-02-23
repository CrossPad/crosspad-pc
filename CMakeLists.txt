cmake_minimum_required(VERSION 3.14)
project(lvgl C CXX)

# Set the correct FreeRTOS port for the current compiler
if(MSVC OR MINGW)
    set(FREERTOS_PORT MSVC_MINGW CACHE STRING "FreeRTOS port" FORCE)
else()
    set(FREERTOS_PORT GCC_POSIX CACHE STRING "FreeRTOS port" FORCE)
endif()

set(LVGL_PRO_PROJECT_DIR
    ""
    CACHE PATH "Path to LVGL Pro Project folder")

option(USE_FREERTOS "Enable FreeRTOS" OFF) # Turn this on to enable FreeRTOS
option(USE_MIDI "Enable MIDI input/output via RtMidi" ON)
option(USE_AUDIO "Enable system audio output via RtAudio" ON)

if(USE_FREERTOS)
    message(STATUS "FreeRTOS is enabled")

    # FreeRTOS integration when USE_FREERTOS is enabled
    add_library(freertos_config INTERFACE)
    target_include_directories(freertos_config SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/config)
    target_compile_definitions(freertos_config INTERFACE projCOVERAGE_TEST=0)

    # Add FreeRTOS as a subdirectory
    add_subdirectory(FreeRTOS)

    # FreeRTOS-specific include directories
    include_directories(${PROJECT_SOURCE_DIR}/FreeRTOS/include)
    include_directories(${PROJECT_SOURCE_DIR}/config)

    if(MSVC OR MINGW)
        set(FREERTOS_PORT_DIR ${PROJECT_SOURCE_DIR}/FreeRTOS/portable/MSVC-MingW)
    else()
        set(FREERTOS_PORT_DIR ${PROJECT_SOURCE_DIR}/FreeRTOS/portable/ThirdParty/GCC/Posix)
    endif()
    include_directories(${FREERTOS_PORT_DIR})

    # Add FreeRTOS sources
    file(GLOB FREERTOS_SOURCES
        "${PROJECT_SOURCE_DIR}/FreeRTOS/*.c"
        "${PROJECT_SOURCE_DIR}/FreeRTOS/portable/MemMang/heap_4.c"
        "${FREERTOS_PORT_DIR}/*.c"
    )
else()
    message(STATUS "FreeRTOS is disabled")
    set(FREERTOS_SOURCES "")  # No FreeRTOS sources if FreeRTOS is disabled
endif()

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

# ── crosspad-core portable sources (excluding ESP-IDF / FreeRTOS specific) ──
set(CROSSPAD_CORE_SOURCES
    crosspad-core/src/app/AppRegistry.cpp
    crosspad-core/src/music/MusicTheory.cpp
    crosspad-core/src/pad/PadManager.cpp
    crosspad-core/src/pad/PadLedController.cpp
    crosspad-core/src/pad/PadAnimator.cpp
    crosspad-core/src/pad/RGBCallbackManager.cpp
    crosspad-core/src/settings/CrosspadSettings.cpp
    crosspad-core/src/protocol/Stm32MessageHandler.cpp
)

# ── crosspad-gui sources ──
set(CROSSPAD_GUI_SOURCES
    crosspad-gui/src/platform/GuiPlatform.cpp
    crosspad-gui/src/styles/styles.cpp
    crosspad-gui/src/widgets/gui_helpers.cpp
    crosspad-gui/src/widgets/keypad_buttons.cpp
    crosspad-gui/src/widgets/footer_button.cpp
    crosspad-gui/src/widgets/spinbox_helpers.cpp
    crosspad-gui/src/widgets/settings_widgets.cpp
    crosspad-gui/src/components/dfu_panel.cpp
    crosspad-gui/src/components/file_explorer.cpp
    crosspad-gui/src/components/radial_menu.cpp
    crosspad-gui/src/components/status_bar.cpp
    crosspad-gui/src/components/ui_modal.cpp
    crosspad-gui/src/components/ui_toast.cpp
    crosspad-gui/src/components/vu_meter.cpp
    crosspad-gui/src/components/app_launcher.cpp
    crosspad-gui/src/components/app_lifecycle.cpp
    crosspad-gui/src/components/volume_overlay.cpp
)

# ── PC platform stubs (event bus, singletons, interface impls) ──
set(PC_STUB_SOURCES
    src/pc_stubs/PcEventBus.cpp
    src/pc_stubs/PcPlatformStubs.cpp
    src/pc_stubs/PcApp.cpp
)

set(MAIN_SOURCES src/mouse_cursor_icon.c src/hal/hal.c
    src/stm32_emu/Stm32EmuWindow.cpp
    src/stm32_emu/EmuEncoder.cpp
    src/stm32_emu/EmuPadGrid.cpp
    src/stm32_emu/EmuPowerButton.cpp
    src/stm32_emu/EmuJackPanel.cpp
    src/crosspad_app.cpp
    ${CROSSPAD_CORE_SOURCES}
    ${CROSSPAD_GUI_SOURCES}
    ${PC_STUB_SOURCES}
)
set(MAIN_LIBS lvgl lvgl::examples lvgl::demos lvgl::thorvg ${SDL2_LIBRARIES} ArduinoJson)

# Create the main executable, depending on the FreeRTOS option
if(USE_FREERTOS)
    if(MSVC OR MINGW)
        list(APPEND MAIN_SOURCES src/freertos_main.cpp src/freertos/freertos_win32_port.c ${FREERTOS_SOURCES})
    else()
        list(APPEND MAIN_SOURCES src/freertos_main.cpp src/freertos/freertos_posix_port.c ${FREERTOS_SOURCES})
    endif()
    list(APPEND MAIN_LIBS freertos_config freertos_kernel)
else()
    list(APPEND MAIN_SOURCES src/main.cpp)
endif()


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

# Audio sources and libraries
if(USE_AUDIO)
    list(APPEND MAIN_SOURCES src/audio/PcAudio.cpp src/audio/PcAudioInput.cpp)
    list(APPEND MAIN_LIBS rtaudio)

    # ML_SynthTools vendored FM synth engine
    set(ML_SYNTH_SOURCES
        lib/ml_synth/ml_fm.cpp
        lib/ml_synth/ml_status_stub.cpp
        lib/ml_synth/ml_utils_stub.cpp
    )

    # ML Piano app (own CMakeLists.txt exports ML_PIANO_APP_SOURCES)
    add_subdirectory(src/apps/ml_piano)

    # Synth engine wrapper (platform-level, like ESP32's audio codec)
    set(ML_PIANO_SYNTH_SOURCES src/synth/MlPianoSynth.cpp)

    list(APPEND MAIN_SOURCES ${ML_SYNTH_SOURCES} ${ML_PIANO_SYNTH_SOURCES} ${ML_PIANO_APP_SOURCES})
endif()

# Custom target to run the executable
add_custom_target(run COMMAND ${EXECUTABLE_OUTPUT_PATH}/main DEPENDS main)

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

add_executable(main ${MAIN_SOURCES})
target_compile_definitions(main PRIVATE
    LV_CONF_INCLUDE_SIMPLE
    USE_LVGL=1
    CP_LCD_HOR_RES=320
    CP_LCD_VER_RES=240
    PLATFORM_PC=1
)
target_include_directories(main PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/crosspad-core/include
    ${PROJECT_SOURCE_DIR}/crosspad-gui/include
    ${PROJECT_SOURCE_DIR}/lib/ml_synth
)
target_link_libraries(main ${MAIN_LIBS})

# MIDI compile definition
if(USE_MIDI)
    target_compile_definitions(main PRIVATE USE_MIDI=1)
endif()

if(USE_AUDIO)
    target_compile_definitions(main PRIVATE USE_AUDIO=1)
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
        target_link_options(main PRIVATE "/SUBSYSTEM:CONSOLE")
    else()
        target_link_options(main PRIVATE "-mconsole")
    endif()
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
