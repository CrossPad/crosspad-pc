/**
 * @file PcBleMidi.cpp
 * @brief BLE MIDI for Linux: SimpleBLE for connection, RtMidi/ALSA for data.
 *
 * On Linux, after SimpleBLE connects a BLE MIDI device, the kernel's btmidi
 * module creates an ALSA sequencer port. We open that port via RtMidi for
 * MIDI I/O — same mechanism as USB MIDI, no GATT characteristic access needed.
 */

#include "PcBleMidi.hpp"
#include "apps/settings/settings_app.h"

#include <simpleble/SimpleBLE.h>
#include <cstdio>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <thread>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static bool uuidMatch(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

/* ── Constructor / Destructor ────────────────────────────────────────── */

PcBleMidi::PcBleMidi() {
    try {
        midiOut_ = std::make_unique<RtMidiOut>();
        midiIn_  = std::make_unique<RtMidiIn>();
    } catch (const RtMidiError& e) {
        printf("[BLE MIDI] RtMidi init error: %s\n", e.what());
    }
}

PcBleMidi::~PcBleMidi() {
    end();
}

/* ── Lifecycle ───────────────────────────────────────────────────────── */

bool PcBleMidi::initAdapter() {
    if (adapter_) return true;

    auto adapters = SimpleBLE::Adapter::get_adapters();
    if (adapters.empty()) {
        printf("[BLE MIDI] No Bluetooth adapters found\n");
        return false;
    }

    adapter_ = std::make_unique<SimpleBLE::Adapter>(std::move(adapters[0]));
    printf("[BLE MIDI] Using adapter: %s [%s]\n",
           adapter_->identifier().c_str(),
           adapter_->address().c_str());
    return true;
}

bool PcBleMidi::begin(crosspad::BleMidiMode mode) {
    end();
    mode_ = mode;

    if (!initAdapter()) return false;

    initialized_.store(true);
    printf("[BLE MIDI] Started in %s mode\n",
           mode == crosspad::BleMidiMode::Host ? "Host (Central)" : "Server (Peripheral)");

    if (mode == crosspad::BleMidiMode::Server) {
        printf("[BLE MIDI] Server mode is not yet supported on PC\n");
        return false;
    }
    return true;
}

void PcBleMidi::end() {
    stopScan();
    disconnect();
    if (scanThread_.joinable()) scanThread_.join();
    adapter_.reset();
    initialized_.store(false);
}

bool PcBleMidi::isConnected() const {
    return connected_.load();
}

crosspad::BleMidiMode PcBleMidi::getMode() const {
    return mode_;
}

/* ── Host mode: Scan ─────────────────────────────────────────────────── */

void PcBleMidi::startScan(uint16_t durationMs) {
    if (!initialized_.load() || scanning_.load()) return;
    if (mode_ != crosspad::BleMidiMode::Host) return;

    {
        std::lock_guard<std::mutex> lock(scanMutex_);
        scanResults_.clear();
        scannedPeripherals_.clear();
    }

    scanning_.store(true);
    printf("[BLE MIDI] Scanning for %u ms...\n", durationMs);

    if (scanThread_.joinable()) scanThread_.join();

    scanThread_ = std::thread([this, durationMs]() {
        try {
            adapter_->set_callback_on_scan_found([this](SimpleBLE::Peripheral peripheral) {
                bool hasMidiService = false;
                for (auto& svc : peripheral.services()) {
                    if (uuidMatch(svc.uuid(), MIDI_SERVICE_UUID)) {
                        hasMidiService = true;
                        break;
                    }
                }
                if (!hasMidiService) return;

                crosspad::BleMidiDevice dev;
                dev.name = peripheral.identifier();
                dev.address = peripheral.address();
                dev.rssi = peripheral.rssi();
                dev.connected = false;
                if (dev.name.empty()) dev.name = "BLE MIDI Device";

                printf("[BLE MIDI] Found: %s (%s) RSSI=%d\n",
                       dev.name.c_str(), dev.address.c_str(), dev.rssi);

                std::lock_guard<std::mutex> lock(scanMutex_);
                auto it = std::find_if(scanResults_.begin(), scanResults_.end(),
                    [&](const crosspad::BleMidiDevice& d) { return d.address == dev.address; });
                if (it == scanResults_.end()) {
                    scanResults_.push_back(std::move(dev));
                    scannedPeripherals_.push_back(peripheral);
                } else {
                    size_t idx = std::distance(scanResults_.begin(), it);
                    *it = std::move(dev);
                    scannedPeripherals_[idx] = peripheral;
                }
            });

            adapter_->scan_for(durationMs);
        } catch (const std::exception& e) {
            printf("[BLE MIDI] Scan error: %s\n", e.what());
        }

        scanning_.store(false);
        printf("[BLE MIDI] Scan complete\n");

        std::lock_guard<std::mutex> lock(scanMutex_);
        dispatchScanResults(scanResults_);
    });
}

void PcBleMidi::stopScan() {
    if (!initialized_.load() || !scanning_.load()) return;
    try { adapter_->scan_stop(); } catch (...) {}
}

bool PcBleMidi::isScanning() const {
    return scanning_.load();
}

std::vector<crosspad::BleMidiDevice> PcBleMidi::getScanResults() const {
    std::lock_guard<std::mutex> lock(scanMutex_);
    return scanResults_;
}

/* ── Host mode: Connect ──────────────────────────────────────────────── */

bool PcBleMidi::connectToDevice(const std::string& address) {
    if (!initialized_.load()) return false;
    if (connected_.load()) disconnect();

    printf("[BLE MIDI] Connecting to %s...\n", address.c_str());

    // Stop any active scan
    if (scanning_.load()) stopScan();
    if (scanThread_.joinable()) scanThread_.join();

    try {
        // On Linux, BLE MIDI is managed by the system Bluetooth daemon (bluez-midi /
        // pipewire-midi-bridge). When a device is already connected, the daemon
        // creates an ALSA sequencer port. We should NOT unpair or re-connect —
        // that disrupts the daemon's GATT notification subscription.
        //
        // Strategy:
        // 1. Check if ALSA port already exists → just open it
        // 2. If not, connect via SimpleBLE → wait for daemon to create port → open it

        std::string deviceName;
        {
            std::lock_guard<std::mutex> lock(scanMutex_);
            for (auto& d : scanResults_) {
                if (d.address == address) { deviceName = d.name; break; }
            }
        }
        if (deviceName.empty()) deviceName = "Crosspad";

        // Step 1: Try opening existing ALSA port (device may already be connected)
        if (openMidiPorts(deviceName)) {
            printf("[BLE MIDI] Using existing ALSA port for %s\n", deviceName.c_str());
        } else {
            // Step 2: Connect via SimpleBLE (triggers daemon to create ALSA port)
            auto peripherals = adapter_->scan_get_results();
            SimpleBLE::Peripheral* targetPtr = nullptr;
            for (auto& p : peripherals) {
                if (p.address() == address) { targetPtr = &p; break; }
            }

            if (!targetPtr) {
                printf("[BLE MIDI] Re-scanning...\n");
                adapter_->scan_for(3000);
                peripherals = adapter_->scan_get_results();
                for (auto& p : peripherals) {
                    if (p.address() == address) { targetPtr = &p; break; }
                }
            }

            if (!targetPtr) {
                printf("[BLE MIDI] Device %s not found\n", address.c_str());
                return false;
            }

            printf("[BLE MIDI] Connecting via SimpleBLE...\n");
            targetPtr->connect();

            if (!targetPtr->is_connected()) {
                printf("[BLE MIDI] Connection failed\n");
                return false;
            }

            peripheral_ = std::make_unique<SimpleBLE::Peripheral>(std::move(*targetPtr));

            peripheral_->set_callback_on_disconnected([this]() {
                connected_.store(false);
                closeMidiPorts();
                printf("[BLE MIDI] Disconnected\n");
                crosspad::BleMidiDevice dev;
                {
                    std::lock_guard<std::mutex> lock(connMutex_);
                    dev = connectedDevice_;
                    connectedDevice_.connected = false;
                }
                dispatchConnectionChanged(false, dev);
            });

            // Wait for ALSA port to appear
            printf("[BLE MIDI] Waiting for ALSA MIDI port...\n");
            bool portFound = false;
            for (int attempt = 0; attempt < 20; attempt++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (openMidiPorts(deviceName)) { portFound = true; break; }
                if (attempt < 3 || attempt % 5 == 0)
                    printf("[BLE MIDI] ALSA port not yet available (attempt %d)\n", attempt + 1);
            }

            if (!portFound) {
                printf("[BLE MIDI] ALSA MIDI port not found after 10s\n");
            }
        }

        // Update state
        {
            std::lock_guard<std::mutex> lock(connMutex_);
            connectedDevice_.name = deviceName;
            connectedDevice_.address = address;
            connectedDevice_.connected = true;
        }

        connected_.store(true);
        printf("[BLE MIDI] Connected to %s (%s)\n", deviceName.c_str(), address.c_str());
        dispatchConnectionChanged(true, connectedDevice_);
        return true;

    } catch (const std::exception& e) {
        printf("[BLE MIDI] Connect error: %s\n", e.what());
        return false;
    }
}

/* ── RtMidi port management ──────────────────────────────────────────── */

bool PcBleMidi::openMidiPorts(const std::string& deviceName) {
    if (!midiOut_ || !midiIn_) return false;

    // Search for ALSA port matching the BLE device name
    unsigned int outCount = midiOut_->getPortCount();
    unsigned int inCount  = midiIn_->getPortCount();

    int outPort = -1, inPort = -1;

    for (unsigned int i = 0; i < outCount; i++) {
        std::string name = midiOut_->getPortName(i);
        // Match by device name substring (case-insensitive-ish)
        if (name.find(deviceName) != std::string::npos ||
            name.find("Bluetooth") != std::string::npos) {
            outPort = i;
            printf("[BLE MIDI] Found ALSA OUT port [%u]: %s\n", i, name.c_str());
            break;
        }
    }

    for (unsigned int i = 0; i < inCount; i++) {
        std::string name = midiIn_->getPortName(i);
        if (name.find(deviceName) != std::string::npos ||
            name.find("Bluetooth") != std::string::npos) {
            inPort = i;
            printf("[BLE MIDI] Found ALSA IN port [%u]: %s\n", i, name.c_str());
            break;
        }
    }

    if (outPort < 0 && inPort < 0) return false;

    try {
        if (outPort >= 0 && !midiOut_->isPortOpen()) {
            midiOut_->openPort(outPort, "CrossPad BLE Out");
            printf("[BLE MIDI] ALSA OUT opened\n");
        }
        if (inPort >= 0 && !midiIn_->isPortOpen()) {
            midiIn_->openPort(inPort, "CrossPad BLE In");
            midiIn_->setCallback(rtMidiCallback, this);
            midiIn_->ignoreTypes(true, true, true);  // ignore sysex, timing, sensing
            printf("[BLE MIDI] ALSA IN opened — receiving MIDI data\n");
        }
        midiPortOpen_ = true;
        return true;
    } catch (const RtMidiError& e) {
        printf("[BLE MIDI] RtMidi port error: %s\n", e.what());
        return false;
    }
}

void PcBleMidi::closeMidiPorts() {
    {
        std::lock_guard<std::mutex> lock(outMutex_);
        midiPortOpen_ = false;
        try {
            if (midiOut_ && midiOut_->isPortOpen()) midiOut_->closePort();
        } catch (...) {}
    }
    try {
        if (midiIn_ && midiIn_->isPortOpen()) midiIn_->closePort();
    } catch (...) {}
}

/* ── RtMidi input callback ───────────────────────────────────────────── */

void PcBleMidi::rtMidiCallback(double, std::vector<unsigned char>* message, void* userData) {
    auto* self = static_cast<PcBleMidi*>(userData);
    if (message && !message->empty()) {
        // Debug: log every raw byte from ALSA
        printf("[BLE MIDI RAW] %zu bytes:", message->size());
        for (auto b : *message) printf(" %02X", b);
        printf("\n");
        self->handleMidiMessage(*message);
    }
}

void PcBleMidi::recordSent(uint8_t status, uint8_t d1, uint8_t d2) {
    std::lock_guard<std::mutex> lock(echoMutex_);
    auto& slot = echoBuf_[echoHead_ % ECHO_BUF_SIZE];
    slot.d[0] = status; slot.d[1] = d1; slot.d[2] = d2;
    slot.ts = std::chrono::steady_clock::now();
    slot.active = true;
    echoHead_++;
}

bool PcBleMidi::isEcho(const std::vector<unsigned char>& msg) {
    if (msg.size() < 3) return false;
    std::lock_guard<std::mutex> lock(echoMutex_);
    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < ECHO_BUF_SIZE; i++) {
        auto& slot = echoBuf_[i];
        if (!slot.active) continue;
        auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - slot.ts).count();
        if (age > ECHO_WINDOW_MS) { slot.active = false; continue; }
        if (msg[0] == slot.d[0] && msg[1] == slot.d[1] && msg[2] == slot.d[2]) {
            slot.active = false;
            return true;
        }
    }
    return false;
}

void PcBleMidi::handleMidiMessage(std::vector<unsigned char>& msg) {
    if (msg.size() < 2) return;

    // Filter loopback: ALSA may route our own output back to our input
    if (isEcho(msg)) {
        printf("[BLE MIDI] Echo filtered: %02X", msg[0]);
        for (size_t i = 1; i < msg.size(); i++) printf(" %02X", msg[i]);
        printf("\n");
        return;
    }
    printf("[BLE MIDI] RX: %02X", msg[0]);
    for (size_t i = 1; i < msg.size(); i++) printf(" %02X", msg[i]);
    printf("\n");

    uint8_t status = msg[0];
    uint8_t statusType = status & 0xF0;
    uint8_t channel = status & 0x0F;

    switch (statusType) {
        case 0x90:  // Note On
            if (msg.size() >= 3) {
                uint8_t note = msg[1], vel = msg[2];
                if (vel == 0)
                    dispatchNoteOff(channel, note, 0);
                else
                    dispatchNoteOn(channel, note, vel);
            }
            break;
        case 0x80:  // Note Off
            if (msg.size() >= 3)
                dispatchNoteOff(channel, msg[1], msg[2]);
            break;
        case 0xB0:  // CC
            if (msg.size() >= 3)
                dispatchCC(channel, msg[1], msg[2]);
            break;
        default:
            break;
    }
}

/* ── Server mode (stub) ──────────────────────────────────────────────── */

void PcBleMidi::startAdvertising(const std::string& deviceName) {
    (void)deviceName;
    printf("[BLE MIDI] Server mode not yet supported on PC\n");
}

void PcBleMidi::stopAdvertising() {}
bool PcBleMidi::isAdvertising() const { return false; }

/* ── Common ──────────────────────────────────────────────────────────── */

void PcBleMidi::disconnect() {
    closeMidiPorts();

    if (connected_.load()) {
        try {
            if (peripheral_ && peripheral_->is_connected())
                peripheral_->disconnect();
        } catch (const std::exception& e) {
            printf("[BLE MIDI] Disconnect error: %s\n", e.what());
        }
    }

    peripheral_.reset();
    connected_.store(false);

    crosspad::BleMidiDevice dev;
    {
        std::lock_guard<std::mutex> lock(connMutex_);
        dev = connectedDevice_;
        connectedDevice_.connected = false;
    }
    dispatchConnectionChanged(false, dev);
}

crosspad::BleMidiDevice PcBleMidi::getConnectedDevice() const {
    std::lock_guard<std::mutex> lock(connMutex_);
    return connectedDevice_;
}

/* ── IMidiOutput (send via RtMidi/ALSA) ──────────────────────────────── */

void PcBleMidi::sendRawMidi(uint8_t status, uint8_t d1, uint8_t d2) {
    std::lock_guard<std::mutex> lock(outMutex_);
    if (!midiPortOpen_ || !midiOut_ || !midiOut_->isPortOpen()) return;

    try {
        recordSent(status, d1, d2);
        std::vector<unsigned char> msg = {status, d1, d2};
        midiOut_->sendMessage(&msg);
        ble_settings_log_midi_out(status, d1, d2);
    } catch (const RtMidiError& e) {
        printf("[BLE MIDI] Send error: %s\n", e.what());
    }
}

void PcBleMidi::sendRawMidi(uint8_t status, uint8_t d1) {
    std::lock_guard<std::mutex> lock(outMutex_);
    if (!midiPortOpen_ || !midiOut_ || !midiOut_->isPortOpen()) return;

    try {
        std::vector<unsigned char> msg = {status, d1};
        midiOut_->sendMessage(&msg);
        ble_settings_log_midi_out(status, d1, 0);
    } catch (const RtMidiError& e) {
        printf("[BLE MIDI] Send error: %s\n", e.what());
    }
}

void PcBleMidi::sendNoteOn(uint8_t note, uint8_t channel) {
    sendNoteOn(note, 127, channel);
}

void PcBleMidi::sendNoteOff(uint8_t note, uint8_t channel) {
    sendNoteOff(note, 0, channel);
}

void PcBleMidi::sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel) {
    sendRawMidi(0x90 | (channel & 0x0F), note & 0x7F, velocity & 0x7F);
}

void PcBleMidi::sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel) {
    sendRawMidi(0x80 | (channel & 0x0F), note & 0x7F, velocity & 0x7F);
}

void PcBleMidi::sendControlChange(uint8_t controller, uint8_t value, uint8_t channel) {
    sendRawMidi(0xB0 | (channel & 0x0F), controller & 0x7F, value & 0x7F);
}

void PcBleMidi::sendAftertouch(uint8_t note, uint8_t pressure, uint8_t channel) {
    sendRawMidi(0xA0 | (channel & 0x0F), note & 0x7F, pressure & 0x7F);
}

void PcBleMidi::sendProgramChange(uint8_t program, uint8_t channel) {
    sendRawMidi(0xC0 | (channel & 0x0F), program & 0x7F);
}

void PcBleMidi::sendPitchBend(int16_t value, uint8_t channel) {
    uint16_t bend14 = static_cast<uint16_t>(value + 8192);
    sendRawMidi(0xE0 | (channel & 0x0F), bend14 & 0x7F, (bend14 >> 7) & 0x7F);
}
