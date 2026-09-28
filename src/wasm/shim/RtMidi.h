#pragma once
/**
 * RtMidi for the browser twin: the subset crosspad-pc uses (PcMidi), backed
 * by Web MIDI through web_io.cpp. Ports are the page's MIDIInput/MIDIOutput
 * lists, in the order Module.webMidi reports them.
 */
#include <exception>
#include <string>
#include <vector>

class RtMidiError : public std::exception {
public:
    enum Type { WARNING, DEBUG_WARNING, UNSPECIFIED, NO_DEVICES_FOUND, INVALID_DEVICE,
                MEMORY_ERROR, INVALID_PARAMETER, INVALID_USE, DRIVER_ERROR, SYSTEM_ERROR, THREAD_ERROR };
    explicit RtMidiError(const std::string& m = "", Type t = UNSPECIFIED) : msg_(m), type_(t) {}
    const char* what() const noexcept override { return msg_.c_str(); }
    const std::string& getMessage() const { return msg_; }
    Type getType() const { return type_; }
    void printMessage() const {}
private:
    std::string msg_;
    Type type_;
};

class RtMidi {
public:
    enum Api { UNSPECIFIED, MACOSX_CORE, LINUX_ALSA, UNIX_JACK, WINDOWS_MM, RTMIDI_DUMMY, WEB_MIDI_API, NUM_APIS };
};

class RtMidiIn {
public:
    typedef void (*RtMidiCallback)(double timeStamp, std::vector<unsigned char>* message, void* userData);

    RtMidiIn(RtMidi::Api = RtMidi::UNSPECIFIED, const std::string& = "RtMidi Input Client", unsigned int = 100);
    ~RtMidiIn();
    unsigned int getPortCount();
    std::string getPortName(unsigned int portNumber = 0);
    void openPort(unsigned int portNumber = 0, const std::string& = "RtMidi Input");
    void openVirtualPort(const std::string& = "RtMidi Input") {}
    void closePort();
    bool isPortOpen() const { return port_ >= 0; }
    void setCallback(RtMidiCallback callback, void* userData = nullptr) { cb_ = callback; ud_ = userData; }
    void cancelCallback() { cb_ = nullptr; ud_ = nullptr; }
    void ignoreTypes(bool midiSysex = true, bool midiTime = true, bool midiSense = true) {
        ignSysex_ = midiSysex; ignTime_ = midiTime; ignSense_ = midiSense;
    }

    // web_io.cpp
    int port_ = -1;
    RtMidiCallback cb_ = nullptr;
    void* ud_ = nullptr;
    bool ignSysex_ = true, ignTime_ = true, ignSense_ = true;
};

class RtMidiOut {
public:
    RtMidiOut(RtMidi::Api = RtMidi::UNSPECIFIED, const std::string& = "RtMidi Output Client") {}
    ~RtMidiOut() { closePort(); }
    unsigned int getPortCount();
    std::string getPortName(unsigned int portNumber = 0);
    void openPort(unsigned int portNumber = 0, const std::string& = "RtMidi Output");
    void openVirtualPort(const std::string& = "RtMidi Output") {}
    void closePort() { port_ = -1; }
    bool isPortOpen() const { return port_ >= 0; }
    void sendMessage(const std::vector<unsigned char>* message) {
        if (message) sendMessage(message->data(), message->size());
    }
    void sendMessage(const unsigned char* message, size_t size);

    int port_ = -1;
};
