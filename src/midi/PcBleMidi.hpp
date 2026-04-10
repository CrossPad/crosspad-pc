#pragma once

/**
 * @file PcBleMidi.hpp
 * @brief SimpleBLE-backed BLE MIDI implementation for PC simulator.
 *
 * Mirrors PcMidi structure: implements crosspad::IBleMidi (which extends
 * IMidiOutput) for integration with MidiInputHandler routing. Uses
 * SimpleBLE library for cross-platform BLE access (Linux/Mac/Windows).
 *
 * Host mode (Central): scan for BLE MIDI peripherals, connect, subscribe
 * to MIDI characteristic notifications, send MIDI via characteristic writes.
 *
 * Server mode (Peripheral): advertise as BLE MIDI device — stubbed for now
 * (SimpleBLE peripheral support is limited on some platforms).
 */

#include <crosspad/midi/IBleMidi.hpp>
#include <RtMidi.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward declarations — avoid SimpleBLE header pollution
namespace SimpleBLE {
    class Adapter;
    class Peripheral;
}

/**
 * On Linux, BLE MIDI data flows through ALSA (kernel btmidi module),
 * NOT through SimpleBLE GATT. After SimpleBLE connects a BLE MIDI device,
 * the kernel creates an ALSA sequencer port. We open that port via RtMidi
 * for actual MIDI data I/O — same mechanism as USB MIDI.
 *
 * SimpleBLE is used only for: scan, connect, disconnect.
 * RtMidi is used for: MIDI input/output data.
 */
class PcBleMidi : public crosspad::IBleMidi {
public:
    PcBleMidi();
    ~PcBleMidi() override;

    PcBleMidi(const PcBleMidi&) = delete;
    PcBleMidi& operator=(const PcBleMidi&) = delete;

    // ── IBleMidi lifecycle ──────────────────────────────────────────
    bool begin(crosspad::BleMidiMode mode) override;
    void end() override;
    bool isConnected() const override;
    crosspad::BleMidiMode getMode() const override;

    // ── Host mode (Central) ─────────────────────────────────────────
    void startScan(uint16_t durationMs) override;
    void stopScan() override;
    bool isScanning() const override;
    std::vector<crosspad::BleMidiDevice> getScanResults() const override;
    bool connectToDevice(const std::string& address) override;

    // ── Server mode (Peripheral) — stub ─────────────────────────────
    void startAdvertising(const std::string& deviceName) override;
    void stopAdvertising() override;
    bool isAdvertising() const override;

    // ── Common ──────────────────────────────────────────────────────
    void disconnect() override;
    crosspad::BleMidiDevice getConnectedDevice() const override;

    // ── IMidiOutput (send MIDI over BLE via RtMidi/ALSA) ────────────
    void sendNoteOn(uint8_t note, uint8_t channel) override;
    void sendNoteOff(uint8_t note, uint8_t channel) override;
    void sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) override;
    void sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) override;
    void sendControlChange(uint8_t controller, uint8_t value, uint8_t channel) override;
    void sendAftertouch(uint8_t note, uint8_t pressure, uint8_t channel) override;
    void sendProgramChange(uint8_t program, uint8_t channel) override;
    void sendPitchBend(int16_t value, uint8_t channel) override;

private:
    // BLE MIDI Service UUID (for scan filtering)
    static constexpr const char* MIDI_SERVICE_UUID = "03b80e5a-ede8-4b33-a751-6ce34ec4c700";

    // SimpleBLE objects (scan + connection management only)
    std::unique_ptr<SimpleBLE::Adapter>    adapter_;
    std::unique_ptr<SimpleBLE::Peripheral> peripheral_;

    // RtMidi objects (MIDI data I/O via ALSA on Linux)
    std::unique_ptr<RtMidiOut> midiOut_;
    std::unique_ptr<RtMidiIn>  midiIn_;
    bool midiPortOpen_ = false;

    // Anti-loopback: time-based filter. ALSA may route our own output
    // back to our input. Only suppress if identical message arrives
    // within ECHO_WINDOW_MS of sending.
    static constexpr int ECHO_WINDOW_MS = 50;
    static constexpr size_t ECHO_BUF_SIZE = 8;
    struct EchoEntry {
        uint8_t d[3];
        std::chrono::steady_clock::time_point ts;
        bool active = false;
    };
    EchoEntry echoBuf_[ECHO_BUF_SIZE] = {};
    size_t echoHead_ = 0;
    std::mutex echoMutex_;
    void recordSent(uint8_t status, uint8_t d1, uint8_t d2);
    bool isEcho(const std::vector<unsigned char>& msg);

    // State
    crosspad::BleMidiMode mode_ = crosspad::BleMidiMode::Host;
    std::atomic<bool> connected_{false};
    std::atomic<bool> scanning_{false};
    std::atomic<bool> initialized_{false};

    // Scan results
    mutable std::mutex scanMutex_;
    std::vector<crosspad::BleMidiDevice> scanResults_;
    std::vector<SimpleBLE::Peripheral> scannedPeripherals_;
    std::thread scanThread_;

    // Connected device info
    mutable std::mutex connMutex_;
    crosspad::BleMidiDevice connectedDevice_;

    // Thread-safe output
    std::mutex outMutex_;

    // RtMidi helpers
    bool openMidiPorts(const std::string& deviceName);
    void closeMidiPorts();
    void sendRawMidi(uint8_t status, uint8_t d1, uint8_t d2);
    void sendRawMidi(uint8_t status, uint8_t d1);

    // RtMidi input callback
    static void rtMidiCallback(double timestamp, std::vector<unsigned char>* message, void* userData);
    void handleMidiMessage(std::vector<unsigned char>& message);

    // SimpleBLE adapter init
    bool initAdapter();
};
