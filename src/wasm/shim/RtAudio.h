#pragma once
/**
 * RtAudio for the browser twin: the subset crosspad-pc uses, backed by
 * WebAudio through web_io.cpp. One output device (the page's AudioContext)
 * and one input device (getUserMedia), both stereo at WEB_AUDIO_RATE. The page
 * pulls audio: each ScriptProcessor block runs the registered producers (the
 * audio module's process()) and then every running stream's callback.
 */
#include <functional>
#include <string>
#include <vector>

typedef unsigned long RtAudioFormat;
static const RtAudioFormat RTAUDIO_SINT8 = 0x1;
static const RtAudioFormat RTAUDIO_SINT16 = 0x2;
static const RtAudioFormat RTAUDIO_SINT24 = 0x4;
static const RtAudioFormat RTAUDIO_SINT32 = 0x8;
static const RtAudioFormat RTAUDIO_FLOAT32 = 0x10;
static const RtAudioFormat RTAUDIO_FLOAT64 = 0x20;

typedef unsigned int RtAudioStreamFlags;
static const RtAudioStreamFlags RTAUDIO_NONINTERLEAVED = 0x1;
static const RtAudioStreamFlags RTAUDIO_MINIMIZE_LATENCY = 0x2;
static const RtAudioStreamFlags RTAUDIO_HOG_DEVICE = 0x4;
static const RtAudioStreamFlags RTAUDIO_SCHEDULE_REALTIME = 0x8;
static const RtAudioStreamFlags RTAUDIO_ALSA_USE_DEFAULT = 0x10;
static const RtAudioStreamFlags RTAUDIO_JACK_DONT_CONNECT = 0x20;

typedef unsigned int RtAudioStreamStatus;
static const RtAudioStreamStatus RTAUDIO_INPUT_OVERFLOW = 0x1;
static const RtAudioStreamStatus RTAUDIO_OUTPUT_UNDERFLOW = 0x2;

typedef int (*RtAudioCallback)(void* outputBuffer, void* inputBuffer, unsigned int nFrames,
                               double streamTime, RtAudioStreamStatus status, void* userData);

enum RtAudioErrorType {
    RTAUDIO_NO_ERROR = 0, RTAUDIO_WARNING, RTAUDIO_UNKNOWN_ERROR, RTAUDIO_NO_DEVICES_FOUND,
    RTAUDIO_INVALID_DEVICE, RTAUDIO_DEVICE_DISCONNECT, RTAUDIO_MEMORY_ERROR,
    RTAUDIO_INVALID_PARAMETER, RTAUDIO_INVALID_USE, RTAUDIO_DRIVER_ERROR,
    RTAUDIO_SYSTEM_ERROR, RTAUDIO_THREAD_ERROR
};

typedef std::function<void(RtAudioErrorType type, const std::string& errorText)> RtAudioErrorCallback;

#define WEB_AUDIO_RATE 48000u
#define WEB_AUDIO_OUT_ID 1u
#define WEB_AUDIO_IN_ID 2u

class RtAudio {
public:
    enum Api { UNSPECIFIED, MACOSX_CORE, LINUX_ALSA, UNIX_JACK, LINUX_PULSE, LINUX_OSS,
               WINDOWS_ASIO, WINDOWS_WASAPI, WINDOWS_DS, RTAUDIO_DUMMY, NUM_APIS };

    struct DeviceInfo {
        unsigned int ID = 0;
        std::string name;
        unsigned int outputChannels = 0;
        unsigned int inputChannels = 0;
        unsigned int duplexChannels = 0;
        bool isDefaultOutput = false;
        bool isDefaultInput = false;
        std::vector<unsigned int> sampleRates;
        unsigned int currentSampleRate = 0;
        unsigned int preferredSampleRate = 0;
        RtAudioFormat nativeFormats = 0;
    };
    struct StreamParameters {
        unsigned int deviceId = 0;
        unsigned int nChannels = 0;
        unsigned int firstChannel = 0;
    };
    struct StreamOptions {
        RtAudioStreamFlags flags = 0;
        unsigned int numberOfBuffers = 0;
        std::string streamName;
        int priority = 0;
    };

    explicit RtAudio(Api = UNSPECIFIED, RtAudioErrorCallback&& = 0) {}
    ~RtAudio() { closeStream(); }

    static std::string getApiName(Api) { return "web"; }
    static std::string getApiDisplayName(Api) { return "Web Audio"; }
    Api getCurrentApi() { return UNSPECIFIED; }
    unsigned int getDeviceCount();
    std::vector<unsigned int> getDeviceIds();
    std::vector<std::string> getDeviceNames();
    DeviceInfo getDeviceInfo(unsigned int deviceId);
    unsigned int getDefaultOutputDevice() { return WEB_AUDIO_OUT_ID; }
    unsigned int getDefaultInputDevice() { return WEB_AUDIO_IN_ID; }

    RtAudioErrorType openStream(StreamParameters* outputParameters, StreamParameters* inputParameters,
                                RtAudioFormat format, unsigned int sampleRate, unsigned int* bufferFrames,
                                RtAudioCallback callback, void* userData = nullptr,
                                StreamOptions* options = nullptr);
    void closeStream();
    RtAudioErrorType startStream() { if (!open_) return RTAUDIO_INVALID_USE; running_ = true; return RTAUDIO_NO_ERROR; }
    RtAudioErrorType stopStream() { running_ = false; return RTAUDIO_NO_ERROR; }
    RtAudioErrorType abortStream() { running_ = false; return RTAUDIO_NO_ERROR; }
    const char* getErrorText() { return error_.c_str(); }
    bool isStreamOpen() const { return open_; }
    bool isStreamRunning() const { return running_; }
    double getStreamTime() { return time_; }
    void setStreamTime(double t) { time_ = t; }
    long getStreamLatency() { return 0; }
    unsigned int getStreamSampleRate() { return WEB_AUDIO_RATE; }
    void showWarnings(bool = true) {}

    // web_io.cpp
    bool open_ = false, running_ = false;
    bool out_ = false, in_ = false;
    RtAudioCallback cb_ = nullptr;
    void* ud_ = nullptr;
    unsigned int frames_ = 0;
    double time_ = 0;
    std::string error_;
};
