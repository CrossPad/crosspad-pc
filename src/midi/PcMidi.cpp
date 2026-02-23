/**
 * @file PcMidi.cpp
 * @brief RtMidi-based MIDI I/O for CrossPad PC simulator
 *
 * Provides Arduino MIDI Library-like API (begin, sendNoteOn, setHandleNoteOn)
 * backed by RtMidi. Implements crosspad::IMidiOutput for crosspad-core integration.
 */

#include "PcMidi.hpp"
#include <cstdio>
#include <algorithm>
#include <cctype>

// ── Construction / Destruction ───────────────────────────────────────────

PcMidi::PcMidi() = default;

PcMidi::~PcMidi()
{
    end();
}

// ── Lifecycle ────────────────────────────────────────────────────────────

bool PcMidi::begin(unsigned int outPort, unsigned int inPort)
{
    bool anyOpen = false;

    printf("[MIDI] Initializing MIDI subsystem (RtMidi)...\n");

    // --- Output ---
    try {
        midiOut_ = std::make_unique<RtMidiOut>();
    } catch (RtMidiError& e) {
        printf("[MIDI] Failed to create RtMidiOut: %s\n", e.what());
    }

    if (midiOut_) {
        unsigned int count = midiOut_->getPortCount();
        printf("[MIDI] Available OUTPUT ports (%u):\n", count);
        for (unsigned int i = 0; i < count; i++) {
            printf("  [%u] %s\n", i, midiOut_->getPortName(i).c_str());
        }

        if (count > 0 && outPort < count) {
            try {
                midiOut_->openPort(outPort);
                outputOpen_ = true;
                anyOpen = true;
                printf("[MIDI] Output opened: [%u] %s\n", outPort, midiOut_->getPortName(outPort).c_str());
            } catch (RtMidiError& e) {
                printf("[MIDI] Failed to open output port %u: %s\n", outPort, e.what());
            }
        } else if (count == 0) {
            printf("[MIDI] No output ports available.\n");
        }
    }

    // --- Input ---
    try {
        midiIn_ = std::make_unique<RtMidiIn>();
    } catch (RtMidiError& e) {
        printf("[MIDI] Failed to create RtMidiIn: %s\n", e.what());
    }

    if (midiIn_) {
        unsigned int count = midiIn_->getPortCount();
        printf("[MIDI] Available INPUT ports (%u):\n", count);
        for (unsigned int i = 0; i < count; i++) {
            printf("  [%u] %s\n", i, midiIn_->getPortName(i).c_str());
        }

        if (count > 0 && inPort < count) {
            try {
                midiIn_->openPort(inPort);
                midiIn_->setCallback(&PcMidi::rtMidiCallback, this);
                // Ignore SysEx by default (can be changed by user via setHandleSystemExclusive)
                midiIn_->ignoreTypes(true, true, true);
                inputOpen_ = true;
                anyOpen = true;
                printf("[MIDI] Input opened: [%u] %s\n", inPort, midiIn_->getPortName(inPort).c_str());
            } catch (RtMidiError& e) {
                printf("[MIDI] Failed to open input port %u: %s\n", inPort, e.what());
            }
        } else if (count == 0) {
            printf("[MIDI] No input ports available.\n");
        }
    }

    // Re-enable SysEx if a handler was already registered (persistent across reconnects)
    if (midiIn_ && inputOpen_ && sysExCb_) {
        midiIn_->ignoreTypes(false, true, true);
    }

    if (!anyOpen) {
        printf("[MIDI] WARNING: No MIDI ports could be opened.\n");
    }

    return anyOpen;
}

bool PcMidi::beginAutoConnect(const std::string& keyword)
{
    autoConnectKeyword_ = keyword;
    autoConnectFound_   = false;

    // Case-insensitive substring search helper
    auto containsIgnoreCase = [](const std::string& haystack, const std::string& needle) -> bool {
        if (needle.empty()) return true;
        std::string h = haystack;
        std::string n = needle;
        std::transform(h.begin(), h.end(), h.begin(), ::tolower);
        std::transform(n.begin(), n.end(), n.begin(), ::tolower);
        return h.find(n) != std::string::npos;
    };

    // Find best matching output port
    unsigned int outPort = 0;
    bool outFound = false;
    {
        RtMidiOut probe;
        unsigned int count = probe.getPortCount();
        for (unsigned int i = 0; i < count; i++) {
            if (containsIgnoreCase(probe.getPortName(i), keyword)) {
                outPort = i;
                outFound = true;
                printf("[MIDI] Auto-connect: found output port [%u] '%s'\n",
                       i, probe.getPortName(i).c_str());
                break;
            }
        }
        if (!outFound && count > 0)
            printf("[MIDI] Auto-connect: no '%s' output found, using port 0\n", keyword.c_str());
    }

    // Find best matching input port
    unsigned int inPort = 0;
    bool inFound = false;
    {
        RtMidiIn probe;
        unsigned int count = probe.getPortCount();
        for (unsigned int i = 0; i < count; i++) {
            if (containsIgnoreCase(probe.getPortName(i), keyword)) {
                inPort = i;
                inFound = true;
                printf("[MIDI] Auto-connect: found input port  [%u] '%s'\n",
                       i, probe.getPortName(i).c_str());
                break;
            }
        }
        if (!inFound && count > 0)
            printf("[MIDI] Auto-connect: no '%s' input found, using port 0\n", keyword.c_str());
    }

    autoConnectFound_ = (outFound || inFound);
    return begin(outPort, inPort);
}

bool PcMidi::reconnect()
{
    printf("[MIDI] Reconnecting (keyword='%s')...\n", autoConnectKeyword_.c_str());
    end();
    return beginAutoConnect(autoConnectKeyword_);
}

void PcMidi::end()
{
    if (midiIn_) {
        if (inputOpen_) {
            midiIn_->cancelCallback();
            midiIn_->closePort();
        }
        midiIn_.reset();
        inputOpen_ = false;
    }

    if (midiOut_) {
        if (outputOpen_) {
            midiOut_->closePort();
        }
        midiOut_.reset();
        outputOpen_ = false;
    }

    printf("[MIDI] Shutdown complete.\n");
}

// ── Output (Arduino MIDI Library signature) ──────────────────────────────

void PcMidi::sendNoteOn(uint8_t note, uint8_t velocity, uint8_t channel)
{
    if (!outputOpen_ || !midiOut_) return;

    std::vector<unsigned char> msg = {
        static_cast<unsigned char>(0x90 | (channel & 0x0F)),
        static_cast<unsigned char>(note & 0x7F),
        static_cast<unsigned char>(velocity & 0x7F)
    };

    std::lock_guard<std::mutex> lock(outMutex_);
    try {
        midiOut_->sendMessage(&msg);
        printf("[MIDI OUT] NoteOn  ch=%u note=%u vel=%u\n",
               (channel & 0x0F) + 1, note, velocity);
    } catch (RtMidiError& e) {
        printf("[MIDI OUT] Send error: %s\n", e.what());
        outputOpen_ = false;  // trigger reconnect timer
    }
}

void PcMidi::sendNoteOff(uint8_t note, uint8_t velocity, uint8_t channel)
{
    if (!outputOpen_ || !midiOut_) return;

    std::vector<unsigned char> msg = {
        static_cast<unsigned char>(0x80 | (channel & 0x0F)),
        static_cast<unsigned char>(note & 0x7F),
        static_cast<unsigned char>(velocity & 0x7F)
    };

    std::lock_guard<std::mutex> lock(outMutex_);
    try {
        midiOut_->sendMessage(&msg);
        printf("[MIDI OUT] NoteOff ch=%u note=%u vel=%u\n",
               (channel & 0x0F) + 1, note, velocity);
    } catch (RtMidiError& e) {
        printf("[MIDI OUT] Send error: %s\n", e.what());
        outputOpen_ = false;  // trigger reconnect timer
    }
}

void PcMidi::sendControlChange(uint8_t cc, uint8_t value, uint8_t channel)
{
    if (!outputOpen_ || !midiOut_) return;

    std::vector<unsigned char> msg = {
        static_cast<unsigned char>(0xB0 | (channel & 0x0F)),
        static_cast<unsigned char>(cc & 0x7F),
        static_cast<unsigned char>(value & 0x7F)
    };

    std::lock_guard<std::mutex> lock(outMutex_);
    try {
        midiOut_->sendMessage(&msg);
        printf("[MIDI OUT] CC     ch=%u cc=%u val=%u\n",
               (channel & 0x0F) + 1, cc, value);
    } catch (RtMidiError& e) {
        printf("[MIDI OUT] Send error: %s\n", e.what());
        outputOpen_ = false;  // trigger reconnect timer
    }
}

// ── crosspad::IMidiOutput overrides ──────────────────────────────────────

void PcMidi::sendNoteOn(uint8_t note, uint8_t channel)
{
    sendNoteOn(note, 127, channel);
}

void PcMidi::sendNoteOff(uint8_t note, uint8_t channel)
{
    sendNoteOff(note, 0, channel);
}

// ── Input callbacks ──────────────────────────────────────────────────────

void PcMidi::setHandleNoteOn(NoteOnCallback cb)
{
    noteOnCb_ = std::move(cb);
}

void PcMidi::setHandleNoteOff(NoteOffCallback cb)
{
    noteOffCb_ = std::move(cb);
}

void PcMidi::setHandleControlChange(ControlChangeCallback cb)
{
    ccCb_ = std::move(cb);
}

void PcMidi::setHandleSystemExclusive(SysExCallback cb)
{
    sysExCb_ = std::move(cb);
    // Enable SysEx reception when a handler is set
    if (midiIn_ && inputOpen_) {
        midiIn_->ignoreTypes(false, true, true);
    }
}

// ── Port management ──────────────────────────────────────────────────────

unsigned int PcMidi::getOutputPortCount() const
{
    return midiOut_ ? midiOut_->getPortCount() : 0;
}

unsigned int PcMidi::getInputPortCount() const
{
    return midiIn_ ? midiIn_->getPortCount() : 0;
}

std::string PcMidi::getOutputPortName(unsigned int index) const
{
    return midiOut_ ? midiOut_->getPortName(index) : "";
}

std::string PcMidi::getInputPortName(unsigned int index) const
{
    return midiIn_ ? midiIn_->getPortName(index) : "";
}

bool PcMidi::isOutputOpen() const
{
    return outputOpen_;
}

bool PcMidi::isInputOpen() const
{
    return inputOpen_;
}

bool PcMidi::isKeywordConnected() const
{
    return autoConnectFound_ && (outputOpen_ || inputOpen_);
}

// ── RtMidi callback (static → instance dispatch) ─────────────────────────

void PcMidi::rtMidiCallback(double timestamp, std::vector<unsigned char>* message, void* userData)
{
    if (!message || message->empty() || !userData) return;
    auto* self = static_cast<PcMidi*>(userData);
    self->handleMidiMessage(timestamp, *message);
}

void PcMidi::handleMidiMessage(double timestamp, std::vector<unsigned char>& message)
{
    (void)timestamp;

    uint8_t status  = message[0];
    uint8_t type    = status & 0xF0;
    uint8_t channel = status & 0x0F;

    // SysEx (F0 ... F7)
    if (status == 0xF0) {
        if (sysExCb_) {
            sysExCb_(message.data(), static_cast<unsigned>(message.size()));
        }
        return;
    }

    // Channel voice messages (need at least 3 bytes)
    if (message.size() < 3) return;

    uint8_t data1 = message[1];
    uint8_t data2 = message[2];

    switch (type) {
    case 0x90: // Note On
        if (data2 > 0) {
            if (noteOnCb_) noteOnCb_(channel, data1, data2);
        } else {
            // Note On with velocity 0 = Note Off (per MIDI spec)
            if (noteOffCb_) noteOffCb_(channel, data1, data2);
        }
        break;

    case 0x80: // Note Off
        if (noteOffCb_) noteOffCb_(channel, data1, data2);
        break;

    case 0xB0: // Control Change
        if (ccCb_) ccCb_(channel, data1, data2);
        break;

    default:
        break;
    }
}
