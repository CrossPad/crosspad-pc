#pragma once

/**
 * @file pc_platform.h
 * @brief PC platform initialization API
 *
 * Call pc_platform_init() from main() before any crosspad-core or crosspad-gui
 * functions. Optionally call pc_platform_set_midi_output() to wire PcMidi.
 */

#ifdef __cplusplus
extern "C" {
#endif

/// Initialize all PC platform stubs (event bus, settings, pad manager, GUI platform)
void pc_platform_init(void);

#ifdef __cplusplus
}

namespace crosspad { class IMidiOutput; }
namespace crosspad { class IAudioOutput; }
namespace crosspad { class IAudioInput; }
namespace crosspad { class RgbColor; }

/// Replace the default null MIDI output with a real one (e.g. PcMidi)
void pc_platform_set_midi_output(crosspad::IMidiOutput* midi);

/// Replace the default null audio output with a real one (e.g. PcAudioOutput)
void pc_platform_set_audio_output(crosspad::IAudioOutput* audio);

/// Set the second audio output (OUT2)
void pc_platform_set_audio_output_2(crosspad::IAudioOutput* audio);

/// Set an audio input (index 0 = IN1, index 1 = IN2)
void pc_platform_set_audio_input(int index, crosspad::IAudioInput* input);

/// Get an audio input by index
crosspad::IAudioInput* pc_platform_get_audio_input(int index);

/// Read back a LED pixel color (0-15) for the STM32 emulator display
crosspad::RgbColor pc_get_led_color(uint16_t idx);

namespace crosspad { class ISynthEngine; }

/// Set the global synth engine (called from crosspad_app.cpp during init)
void pc_platform_set_synth_engine(crosspad::ISynthEngine* synth);

/// Get the global synth engine (used by MlPianoApp for audio routing)
crosspad::ISynthEngine* pc_platform_get_synth_engine();

/// Save current CrosspadSettings to ~/.crosspad/preferences.json
void pc_platform_save_settings();

/// Get user profile directory path (~/.crosspad)
const char* pc_platform_get_profile_dir();

/// Set the go-home callback (used by ISettingsUI to navigate back to launcher)
void pc_platform_set_go_home(void (*fn)());

// Forward declarations for PC audio devices
class PcAudioOutput;
class PcAudioInput;

/// Get a PC audio output by index (0=OUT1, 1=OUT2). Returns nullptr if invalid.
PcAudioOutput* pc_platform_get_audio_output(int index);

/// Save the current mixer state to ~/.crosspad/mixer_state.json
void pc_platform_save_mixer_state();

#endif
