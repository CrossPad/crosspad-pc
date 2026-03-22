# ── Catch2 test suite for crosspad-core logic ──
# Compiles core sources + FreeRTOS + Null stubs. No SDL2, no LVGL, no GUI.

include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.0
)
FetchContent_MakeAvailable(Catch2)

# ── crosspad-core sources needed by tests (explicit list) ──
set(CORE_TEST_SOURCES
    # Platform init + services
    ${PROJECT_SOURCE_DIR}/crosspad-core/src/platform/CrosspadPlatformInit.cpp
    ${PROJECT_SOURCE_DIR}/crosspad-core/src/platform/PlatformCapabilities.cpp
    ${PROJECT_SOURCE_DIR}/crosspad-core/src/platform/PlatformServices.cpp

    # Event bus (FreeRTOS — same as main target)
    ${PROJECT_SOURCE_DIR}/crosspad-core/src/event/FreeRtosEventBus.cpp

    # Pad system
    ${PROJECT_SOURCE_DIR}/crosspad-core/src/pad/PadManager.cpp
    ${PROJECT_SOURCE_DIR}/crosspad-core/src/pad/PadLedController.cpp
    ${PROJECT_SOURCE_DIR}/crosspad-core/src/pad/PadAnimator.cpp
    ${PROJECT_SOURCE_DIR}/crosspad-core/src/pad/RGBCallbackManager.cpp

    # App system
    ${PROJECT_SOURCE_DIR}/crosspad-core/src/app/AppRegistry.cpp
    ${PROJECT_SOURCE_DIR}/crosspad-core/src/app/AppManagerBase.cpp

    # Settings
    ${PROJECT_SOURCE_DIR}/crosspad-core/src/settings/CrosspadSettings.cpp
    ${PROJECT_SOURCE_DIR}/crosspad-core/src/settings/ISettingsUI.cpp

    # MIDI input routing (for e2e tests)
    ${PROJECT_SOURCE_DIR}/crosspad-core/src/midi/MidiInputHandler.cpp
)

# ── Test sources ──
set(TEST_SOURCES
    test_main.cpp
    test_platform_capabilities.cpp
    test_app_registry.cpp
    test_event_bus.cpp
    test_pad_manager.cpp
    test_settings.cpp
    test_e2e_scenarios.cpp
)

add_executable(crosspad_tests ${TEST_SOURCES} ${CORE_TEST_SOURCES})

target_compile_definitions(crosspad_tests PRIVATE
    PLATFORM_PC=1
    CP_LCD_HOR_RES=320
    CP_LCD_VER_RES=240
    USE_FREERTOS=1
)

target_include_directories(crosspad_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/crosspad-core/include
    ${PROJECT_SOURCE_DIR}/crosspad-gui/include
)

target_link_libraries(crosspad_tests PRIVATE
    Catch2::Catch2
    ArduinoJson
    freertos_config
    freertos_kernel
)

# FreeRTOS heap + port sources (pvPortMalloc, vPortFree, port layer)
# FREERTOS_SOURCES is defined in the top-level CMakeLists.txt and includes
# kernel .c files, heap_4.c, and the platform port .c files.
target_sources(crosspad_tests PRIVATE ${FREERTOS_SOURCES})
if(MSVC OR MINGW)
    target_sources(crosspad_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/freertos/freertos_win32_port.c
    )
    target_include_directories(crosspad_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/FreeRTOS/portable/MSVC-MingW
    )
else()
    target_sources(crosspad_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/freertos/freertos_posix_port.c
    )
    target_include_directories(crosspad_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/FreeRTOS/portable/ThirdParty/GCC/Posix
    )
endif()

include(CTest)
include(Catch)
catch_discover_tests(crosspad_tests)

# ── GUI integration tests (launches full simulator, talks via TCP) ──
# Separate executable — no crosspad-core linkage, just TCP client + Catch2.
# Requires bin/main.exe to be built first.

add_executable(gui_tests test_gui_harness.cpp)
target_compile_definitions(gui_tests PRIVATE PLATFORM_PC=1)
target_link_libraries(gui_tests PRIVATE Catch2::Catch2)
if(WIN32)
    target_link_libraries(gui_tests PRIVATE ws2_32)
endif()

# GUI tests depend on the main simulator being built
add_dependencies(gui_tests CrossPad)

# GUI tests can't use catch_discover_tests (it runs the exe at build time,
# which tries to launch the simulator). Register as a single CTest entry.
# Run with: ctest --test-dir build -L gui
add_test(NAME gui_integration COMMAND gui_tests)
set_tests_properties(gui_integration PROPERTIES
    LABELS "gui"
    TIMEOUT 60
)
