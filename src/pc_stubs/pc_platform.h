#pragma once

/**
 * @file pc_platform.h
 * @brief PC platform initialization API
 *
 * Call pc_platform_init() from main() before any crosspad-core or crosspad-gui
 * functions. Use getPlatformServices() setters (from PlatformServices.hpp) to
 * swap MIDI, audio, and synth at runtime.
 */

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize all PC platform stubs (event bus, settings, pad manager, GUI platform)
void pc_platform_init(void);

#ifdef __cplusplus
}

namespace crosspad { class IAudioOutput; }
namespace crosspad { class IAudioInput; }
namespace crosspad { class ISynthEngine; }
namespace crosspad { class RgbColor; }

// ── PC-specific device management (dual outputs/inputs) ─────────────────

/// Set the second audio output (OUT2) — PC mixer-specific
void pc_platform_set_audio_output_2(crosspad::IAudioOutput* audio);

/// Set an audio input (index 0 = IN1, index 1 = IN2)
void pc_platform_set_audio_input(int index, crosspad::IAudioInput* input);

/// Get an audio input by index
crosspad::IAudioInput* pc_platform_get_audio_input(int index);

/// Read back a LED pixel color (0-15) for the STM32 emulator display
crosspad::RgbColor pc_get_led_color(uint16_t idx);

/// Get the global synth engine (delegates to PlatformServices)
crosspad::ISynthEngine* pc_platform_get_synth_engine();

/// Initialize PC HTTP client (WinHTTP on Windows, no-op on other platforms)
void pc_http_client_init();

/// Save current CrosspadSettings to ~/.crosspad/preferences.json
void pc_platform_save_settings();

/// Read whether auto-update check is enabled (default: true)
bool pc_platform_get_auto_check_updates();

/// Set whether auto-update check is enabled (persisted immediately)
void pc_platform_set_auto_check_updates(bool enabled);

/// Get user profile directory path (~/.crosspad)
const char* pc_platform_get_profile_dir();

/// Set the go-home callback (used by ISettingsUI to navigate back to launcher)
void pc_platform_set_go_home(void (*fn)());

// Forward declarations for PC audio devices
class PcAudioOutput;

/// Get a PC audio output by index (0=OUT1, 1=OUT2). Returns nullptr if invalid.
PcAudioOutput* pc_platform_get_audio_output(int index);

/// Save the current mixer state to ~/.crosspad/mixer_state.json
void pc_platform_save_mixer_state();

// ── Virtual SD card ──────────────────────────────────────────────────────

#include <string>

/// Set the virtual SD card root directory. Creates /crosspad/kits/ and
/// /crosspad/recordings/ subdirectories if they don't exist.
/// Pass empty string to unmount.
void pc_platform_set_sdcard_path(const std::string& path);

/// Get the current virtual SD card root directory, or empty if not mounted.
const std::string& pc_platform_get_sdcard_path();

/// Resolve a virtual path (e.g. "/crosspad/kits/...") to the actual
/// filesystem path under the mounted SD card root. Returns input unchanged
/// if SD card is not mounted or path doesn't start with "/crosspad".
std::string pc_platform_resolve_sdcard_path(const std::string& virtualPath);

#endif
