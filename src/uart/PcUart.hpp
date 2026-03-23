#pragma once

/**
 * @file PcUart.hpp
 * @brief Virtual USB/UART with COM port backend for the CrossPad PC emulator.
 *
 * Provides OTG-like functionality: the simulator can act as either a UART
 * host (reading data from a connected device) or device (writing data out),
 * or both simultaneously over the same COM port. Uses Windows serial API.
 *
 * Received data is stored in a thread-safe ring buffer and can be polled
 * by the Serial Monitor app or other consumers.
 */

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <thread>
#include <cstdint>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef ERROR
#endif

class PcUart {
public:
    /// Standard baud rates
    enum class BaudRate : uint32_t {
        B9600   = 9600,
        B19200  = 19200,
        B38400  = 38400,
        B57600  = 57600,
        B115200 = 115200,
        B230400 = 230400,
        B460800 = 460800,
        B921600 = 921600
    };

    /// Callback for received data lines (newline-terminated)
    using LineCallback = std::function<void(const std::string& line)>;

    PcUart() = default;
    ~PcUart();

    PcUart(const PcUart&) = delete;
    PcUart& operator=(const PcUart&) = delete;

    /// Open a COM port at the specified baud rate.
    /// @return true if successfully opened
    bool open(const std::string& portName, BaudRate baud = BaudRate::B115200);

    /// Close the current connection.
    void close();

    /// @return true if the COM port is currently open
    bool isOpen() const { return open_.load(); }

    /// @return the name of the currently connected port (empty if not connected)
    std::string getPortName() const;

    /// @return current baud rate
    BaudRate getBaudRate() const { return baudRate_; }

    /// Send raw bytes out the COM port (device/TX mode).
    /// @return number of bytes actually written, or -1 on error
    int write(const uint8_t* data, size_t len);

    /// Send a string (convenience wrapper).
    int write(const std::string& text);

    /// Set callback invoked for each received line (called from reader thread).
    void setOnLineReceived(LineCallback cb);

    /// Read all buffered lines since last call. Thread-safe.
    /// @return vector of received lines (oldest first)
    std::vector<std::string> readLines();

    /// Get total number of lines received since open.
    size_t totalLinesReceived() const { return totalLines_.load(); }

    /// Enumerate available COM ports on the system.
    /// @return vector of port names (e.g., "COM3", "COM4")
    static std::vector<std::string> enumeratePorts();

    /// Find COM ports matching a specific USB VID:PID.
    /// Uses Windows SetupAPI to query device hardware IDs.
    /// @return vector of matching port names
    static std::vector<std::string> findPortsByVidPid(uint16_t vid, uint16_t pid);

    /// CrossPad hardware USB identifiers (ESP32-S3)
    static constexpr uint16_t CROSSPAD_VID = 0x303A;
    static constexpr uint16_t CROSSPAD_PID = 0x1001;

private:
    static constexpr size_t MAX_BUFFERED_LINES = 2000;

#ifdef _WIN32
    HANDLE hSerial_ = INVALID_HANDLE_VALUE;
#endif

    std::string portName_;
    BaudRate baudRate_ = BaudRate::B115200;
    std::atomic<bool> open_{false};
    std::atomic<bool> stopReader_{false};
    std::thread readerThread_;

    // Line buffer (ring)
    mutable std::mutex bufMutex_;
    std::vector<std::string> lineBuffer_;
    std::string partialLine_;  // incomplete line being built
    std::atomic<size_t> totalLines_{0};

    LineCallback lineCb_;

    void readerLoop();
};
