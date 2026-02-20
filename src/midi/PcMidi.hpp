#pragma once

/**
 * @file PcMidi.hpp
 * @brief Arduino MIDI Library-like interface for PC simulator, backed by RtMidi.
 *
 * Mirrors the MidiInterface API used in 2playerCrosspad (begin, sendNoteOn,
 * setHandleNoteOn, etc.) and implements crosspad::IMidiOutput for integration
 * with crosspad-core's PadManager / EventBus.
 *
 * Backend: RtMidi with Windows Multimedia (winmm) on Windows.
 */

#include <crosspad/midi/IMidiOutput.hpp>
#include <RtMidi.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class PcMidi : public crosspad::IMidiOutput {
public:
    // ── Callback types (Arduino MIDI Library pattern) ────────────────────

    using NoteOnCallback        = std::function<void(uint8_t channel, uint8_t note, uint8_t velocity)>;
    using NoteOffCallback       = std::function<void(uint8_t channel, uint8_t note, uint8_t velocity)>;
    using ControlChangeCallback = std::function<void(uint8_t channel, uint8_t cc, uint8_t value)>;
    using SysExCallback         = std::function<void(uint8_t* data, unsigned size)>;

    PcMidi();
    ~PcMidi();

    // Non-copyable
    PcMidi(const PcMidi&) = delete;
    PcMidi& operator=(const PcMidi&) = delete;

    // ── Lifecycle (Arduino-style) ────────────────────────────────────────

    /**
     * @brief Initialize MIDI. Lists available ports and opens specified indices.
     * @param outPort Output port index (default 0 = first available)
     * @param inPort  Input port index (default 0 = first available)
     * @return true if at least one port was opened
     */
    bool begin(unsigned int outPort = 0, unsigned int inPort = 0);

    /// Close all ports and release resources
    void end();

    // ── Output (Arduino MIDI Library signature) ──────────────────────────

    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel);
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel);
    void sendControlChange(uint8_t cc, uint8_t value, uint8_t channel);

    // ── crosspad::IMidiOutput overrides ──────────────────────────────────

    void sendNoteOn(uint8_t note, uint8_t channel) override;   // velocity = 127
    void sendNoteOff(uint8_t note, uint8_t channel) override;  // velocity = 0

    // ── Input callbacks (Arduino-style setHandle*) ───────────────────────

    void setHandleNoteOn(NoteOnCallback cb);
    void setHandleNoteOff(NoteOffCallback cb);
    void setHandleControlChange(ControlChangeCallback cb);
    void setHandleSystemExclusive(SysExCallback cb);

    // ── Port management ──────────────────────────────────────────────────

    unsigned int getOutputPortCount() const;
    unsigned int getInputPortCount() const;
    std::string getOutputPortName(unsigned int index) const;
    std::string getInputPortName(unsigned int index) const;

    bool isOutputOpen() const;
    bool isInputOpen() const;

private:
    std::unique_ptr<RtMidiOut> midiOut_;
    std::unique_ptr<RtMidiIn>  midiIn_;

    bool outputOpen_ = false;
    bool inputOpen_  = false;

    // Callbacks
    NoteOnCallback        noteOnCb_;
    NoteOffCallback       noteOffCb_;
    ControlChangeCallback ccCb_;
    SysExCallback         sysExCb_;

    // Thread safety for output
    std::mutex outMutex_;

    // RtMidi static callback (dispatches to instance)
    static void rtMidiCallback(double timestamp, std::vector<unsigned char>* message, void* userData);
    void handleMidiMessage(double timestamp, std::vector<unsigned char>& message);
};
