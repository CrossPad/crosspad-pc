/**
 * @file PcUart.cpp
 * @brief Serial port backends for the CrossPad virtual USB/UART: Windows COM
 *        ports and Linux ttys (termios).
 */

#include "PcUart.hpp"
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <setupapi.h>
#include <devguid.h>
#pragma comment(lib, "setupapi.lib")

/* ── Enumerate COM ports ─────────────────────────────────────────────── */

std::vector<std::string> PcUart::enumeratePorts()
{
    std::vector<std::string> ports;

    // Query registry for serial comm ports
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DEVICEMAP\\SERIALCOMM",
                      0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return ports;
    }

    char valueName[256];
    char valueData[256];
    DWORD valueNameSize, valueDataSize, type;

    for (DWORD i = 0; ; i++) {
        valueNameSize = sizeof(valueName);
        valueDataSize = sizeof(valueData);
        LONG result = RegEnumValueA(hKey, i, valueName, &valueNameSize,
                                    nullptr, &type, (LPBYTE)valueData, &valueDataSize);
        if (result != ERROR_SUCCESS) break;
        if (type == REG_SZ) {
            ports.emplace_back(valueData);
        }
    }

    RegCloseKey(hKey);
    return ports;
}

/* ── Find COM ports by USB VID:PID ────────────────────────────────────── */

std::vector<std::string> PcUart::findPortsByVidPid(uint16_t vid, uint16_t pid)
{
    std::vector<std::string> result;

    // Build VID/PID search string: "VID_303A&PID_1001"
    char vidPidStr[32];
    snprintf(vidPidStr, sizeof(vidPidStr), "VID_%04X&PID_%04X", vid, pid);

    // Get device info set for all COM ports
    HDEVINFO devInfo = SetupDiGetClassDevsA(
        &GUID_DEVCLASS_PORTS, nullptr, nullptr,
        DIGCF_PRESENT);

    if (devInfo == INVALID_HANDLE_VALUE) return result;

    SP_DEVINFO_DATA devInfoData = {};
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devInfoData); i++) {
        // Check hardware ID for VID/PID match
        char hwId[512] = {};
        if (!SetupDiGetDeviceRegistryPropertyA(
                devInfo, &devInfoData, SPDRP_HARDWAREID,
                nullptr, (PBYTE)hwId, sizeof(hwId), nullptr)) {
            continue;
        }

        // hwId can be multi-sz — check if our VID/PID appears anywhere
        bool match = false;
        for (char* p = hwId; *p; p += strlen(p) + 1) {
            if (strstr(p, vidPidStr)) {
                match = true;
                break;
            }
        }
        if (!match) continue;

        // Get the COM port name from the device's registry key
        HKEY devKey = SetupDiOpenDevRegKey(
            devInfo, &devInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (devKey == INVALID_HANDLE_VALUE) continue;

        char portName[64] = {};
        DWORD portNameSize = sizeof(portName);
        DWORD type = 0;
        if (RegQueryValueExA(devKey, "PortName", nullptr, &type,
                             (LPBYTE)portName, &portNameSize) == ERROR_SUCCESS
            && type == REG_SZ) {
            result.emplace_back(portName);
        }
        RegCloseKey(devKey);
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return result;
}

/* ── Open / Close ────────────────────────────────────────────────────── */

bool PcUart::open(const std::string& portName, BaudRate baud)
{
    close();

    // Windows needs "\\\\.\\COMx" for ports >= COM10
    std::string devPath = "\\\\.\\" + portName;

    hSerial_ = CreateFileA(devPath.c_str(),
                           GENERIC_READ | GENERIC_WRITE,
                           0, nullptr,
                           OPEN_EXISTING,
                           0, nullptr);

    if (hSerial_ == INVALID_HANDLE_VALUE) {
        printf("[UART] Failed to open %s (error %lu)\n", portName.c_str(), GetLastError());
        return false;
    }

    // Configure serial parameters
    DCB dcb = {};
    dcb.DCBlength = sizeof(DCB);
    if (!GetCommState(hSerial_, &dcb)) {
        printf("[UART] GetCommState failed\n");
        CloseHandle(hSerial_);
        hSerial_ = INVALID_HANDLE_VALUE;
        return false;
    }

    dcb.BaudRate = static_cast<DWORD>(baud);
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;

    if (!SetCommState(hSerial_, &dcb)) {
        printf("[UART] SetCommState failed\n");
        CloseHandle(hSerial_);
        hSerial_ = INVALID_HANDLE_VALUE;
        return false;
    }

    // Set timeouts: short read timeout so the reader thread stays responsive
    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout         = 50;
    timeouts.ReadTotalTimeoutConstant    = 100;
    timeouts.ReadTotalTimeoutMultiplier  = 0;
    timeouts.WriteTotalTimeoutConstant   = 100;
    timeouts.WriteTotalTimeoutMultiplier = 10;
    SetCommTimeouts(hSerial_, &timeouts);

    portName_ = portName;
    baudRate_ = baud;
    open_.store(true);
    stopReader_.store(false);

    // Start reader thread
    readerThread_ = std::thread(&PcUart::readerLoop, this);

    printf("[UART] Opened %s @ %u baud\n", portName.c_str(), static_cast<uint32_t>(baud));
    return true;
}

void PcUart::close()
{
    if (!open_.load()) return;

    stopReader_.store(true);
    if (readerThread_.joinable()) {
        readerThread_.join();
    }

    if (hSerial_ != INVALID_HANDLE_VALUE) {
        CloseHandle(hSerial_);
        hSerial_ = INVALID_HANDLE_VALUE;
    }

    printf("[UART] Closed %s\n", portName_.c_str());
    portName_.clear();
    open_.store(false);

    std::lock_guard<std::mutex> lock(bufMutex_);
    lineBuffer_.clear();
    partialLine_.clear();
}

PcUart::~PcUart()
{
    close();
}

/* ── TX (write) ──────────────────────────────────────────────────────── */

int PcUart::write(const uint8_t* data, size_t len)
{
    if (!open_.load() || hSerial_ == INVALID_HANDLE_VALUE) return -1;

    DWORD written = 0;
    if (!WriteFile(hSerial_, data, (DWORD)len, &written, nullptr)) {
        printf("[UART] Write failed (error %lu)\n", GetLastError());
        return -1;
    }
    return (int)written;
}

int PcUart::write(const std::string& text)
{
    return write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

/* ── RX (reader thread) ─────────────────────────────────────────────── */

void PcUart::readerLoop()
{
    uint8_t buf[512];

    while (!stopReader_.load()) {
        DWORD bytesRead = 0;
        if (!ReadFile(hSerial_, buf, sizeof(buf), &bytesRead, nullptr)) {
            if (GetLastError() != ERROR_TIMEOUT) {
                printf("[UART] Read error %lu, closing\n", GetLastError());
                break;
            }
            continue;
        }

        if (bytesRead == 0) continue;

        // Process received bytes into lines
        std::lock_guard<std::mutex> lock(bufMutex_);
        for (DWORD i = 0; i < bytesRead; i++) {
            char c = static_cast<char>(buf[i]);
            if (c == '\n') {
                // Remove trailing \r if present
                if (!partialLine_.empty() && partialLine_.back() == '\r') {
                    partialLine_.pop_back();
                }
                // Ring buffer: drop oldest if full
                if (lineBuffer_.size() >= MAX_BUFFERED_LINES) {
                    lineBuffer_.erase(lineBuffer_.begin());
                }
                lineBuffer_.push_back(partialLine_);
                totalLines_.fetch_add(1);

                // Fire callback (still under lock — callbacks should be fast)
                if (lineCb_) {
                    lineCb_(partialLine_);
                }
                partialLine_.clear();
            } else {
                partialLine_ += c;
            }
        }
    }
}

/* ── Public helpers ──────────────────────────────────────────────────── */

std::string PcUart::getPortName() const
{
    return portName_;
}

void PcUart::setOnLineReceived(LineCallback cb)
{
    std::lock_guard<std::mutex> lock(bufMutex_);
    lineCb_ = std::move(cb);
}

std::vector<std::string> PcUart::readLines()
{
    std::lock_guard<std::mutex> lock(bufMutex_);
    std::vector<std::string> result;
    result.swap(lineBuffer_);
    return result;
}

#elif defined(__linux__)
/* ── Linux: ttys through termios ─────────────────────────────────────── */

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fstream>

/// USB serial adapters and CDC-ACM devices (a CrossPad, its STM bridge).
std::vector<std::string> PcUart::enumeratePorts()
{
    std::vector<std::string> ports;
    DIR* dir = opendir("/dev");
    if (!dir) return ports;
    while (dirent* e = readdir(dir)) {
        if (strncmp(e->d_name, "ttyACM", 6) == 0 || strncmp(e->d_name, "ttyUSB", 6) == 0) {
            ports.push_back(std::string("/dev/") + e->d_name);
        }
    }
    closedir(dir);
    std::sort(ports.begin(), ports.end());
    return ports;
}

/// A hex id file (idVendor/idProduct) of the USB device above a tty.
static bool readHexId(const std::string& path, uint16_t& out)
{
    std::ifstream f(path);
    std::string text;
    if (!(f >> text)) return false;
    out = static_cast<uint16_t>(strtoul(text.c_str(), nullptr, 16));
    return true;
}

std::vector<std::string> PcUart::findPortsByVidPid(uint16_t vid, uint16_t pid)
{
    std::vector<std::string> result;
    for (const auto& port : enumeratePorts()) {
        // /sys/class/tty/<tty>/device is the USB interface; its USB device,
        // the one with idVendor/idProduct, is a directory or two above.
        const std::string link = "/sys/class/tty/" + port.substr(5) + "/device";
        char real[PATH_MAX];
        if (!realpath(link.c_str(), real)) continue;
        std::string dir = real;
        for (int up = 0; up < 3 && !dir.empty(); up++) {
            uint16_t v = 0, p = 0;
            if (readHexId(dir + "/idVendor", v) && readHexId(dir + "/idProduct", p)) {
                if (v == vid && p == pid) result.push_back(port);
                break;
            }
            dir = dir.substr(0, dir.find_last_of('/'));
        }
    }
    return result;
}

/// By value: termios' B9600... macros shadow the BaudRate names in this file.
static speed_t toSpeed(PcUart::BaudRate baud)
{
    switch (static_cast<uint32_t>(baud)) {
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 230400: return B230400;
        case 460800: return B460800;
        case 921600: return B921600;
        default:     return B115200;
    }
}

bool PcUart::open(const std::string& portName, BaudRate baud)
{
    close();

    fd_ = ::open(portName.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd_ < 0) {
        printf("[UART] Failed to open %s (%s)\n", portName.c_str(), strerror(errno));
        return false;
    }

    // Raw 8N1, no flow control; the reader polls, so VMIN/VTIME stay 0.
    termios tio = {};
    if (tcgetattr(fd_, &tio) != 0) {
        printf("[UART] tcgetattr %s failed (%s)\n", portName.c_str(), strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    cfmakeraw(&tio);
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_cflag &= ~(CSTOPB | CRTSCTS);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;
    cfsetispeed(&tio, toSpeed(baud));
    cfsetospeed(&tio, toSpeed(baud));
    if (tcsetattr(fd_, TCSANOW, &tio) != 0) {
        printf("[UART] tcsetattr %s failed (%s)\n", portName.c_str(), strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    tcflush(fd_, TCIFLUSH);

    portName_ = portName;
    baudRate_ = baud;
    open_.store(true);
    stopReader_.store(false);

    readerThread_ = std::thread(&PcUart::readerLoop, this);

    printf("[UART] Opened %s @ %u baud\n", portName.c_str(), static_cast<uint32_t>(baud));
    return true;
}

void PcUart::close()
{
    if (!open_.load()) return;

    stopReader_.store(true);
    if (readerThread_.joinable()) {
        readerThread_.join();
    }

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }

    printf("[UART] Closed %s\n", portName_.c_str());
    portName_.clear();
    open_.store(false);

    std::lock_guard<std::mutex> lock(bufMutex_);
    lineBuffer_.clear();
    partialLine_.clear();
}

PcUart::~PcUart()
{
    close();
}

int PcUart::write(const uint8_t* data, size_t len)
{
    if (!open_.load() || fd_ < 0) return -1;

    size_t done = 0;
    while (done < len) {
        const ssize_t n = ::write(fd_, data + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            printf("[UART] Write failed (%s)\n", strerror(errno));
            return -1;
        }
        done += static_cast<size_t>(n);
    }
    return static_cast<int>(done);
}

int PcUart::write(const std::string& text)
{
    return write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

void PcUart::readerLoop()
{
    uint8_t buf[512];

    while (!stopReader_.load()) {
        pollfd pfd = {fd_, POLLIN, 0};
        const int ready = poll(&pfd, 1, 100);   // short, so close() is not kept waiting
        if (ready < 0) {
            if (errno == EINTR) continue;
            printf("[UART] poll failed (%s), reader stops\n", strerror(errno));
            break;
        }
        if (ready == 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            printf("[UART] %s went away, reader stops\n", portName_.c_str());
            break;
        }

        const ssize_t n = ::read(fd_, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            printf("[UART] Read failed (%s), reader stops\n", strerror(errno));
            break;
        }
        if (n == 0) continue;

        std::lock_guard<std::mutex> lock(bufMutex_);
        for (ssize_t i = 0; i < n; i++) {
            const char c = static_cast<char>(buf[i]);
            if (c == '\n') {
                if (!partialLine_.empty() && partialLine_.back() == '\r') {
                    partialLine_.pop_back();
                }
                if (lineBuffer_.size() >= MAX_BUFFERED_LINES) {
                    lineBuffer_.erase(lineBuffer_.begin());
                }
                lineBuffer_.push_back(partialLine_);
                totalLines_.fetch_add(1);
                if (lineCb_) {
                    lineCb_(partialLine_);
                }
                partialLine_.clear();
            } else {
                partialLine_ += c;
            }
        }
    }
}

std::string PcUart::getPortName() const
{
    return portName_;
}

void PcUart::setOnLineReceived(LineCallback cb)
{
    std::lock_guard<std::mutex> lock(bufMutex_);
    lineCb_ = std::move(cb);
}

std::vector<std::string> PcUart::readLines()
{
    std::lock_guard<std::mutex> lock(bufMutex_);
    std::vector<std::string> result;
    result.swap(lineBuffer_);
    return result;
}

#else
/* ── Stub for other platforms ────────────────────────────────────────── */

std::vector<std::string> PcUart::enumeratePorts() { return {}; }
std::vector<std::string> PcUart::findPortsByVidPid(uint16_t, uint16_t) { return {}; }
bool PcUart::open(const std::string&, BaudRate) { return false; }
void PcUart::close() {}
PcUart::~PcUart() {}
int PcUart::write(const uint8_t*, size_t) { return -1; }
int PcUart::write(const std::string&) { return -1; }
std::string PcUart::getPortName() const { return {}; }
void PcUart::setOnLineReceived(LineCallback) {}
std::vector<std::string> PcUart::readLines() { return {}; }
void PcUart::readerLoop() {}

#endif // _WIN32
