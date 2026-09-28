/**
 * @file    web_uart.cpp
 * @brief   PcUart for the browser twin: Web Serial instead of a COM port.
 *
 * Same header and behaviour as src/uart/PcUart.cpp — ports are the ones the
 * page was granted (navigator.serial), named "Web Serial N (vid:pid)"; bytes
 * arrive through wasm_uart_rx() and are split into lines exactly as the
 * reader thread did.
 */
#include "uart/PcUart.hpp"

#include <emscripten.h>

#include <cstdio>
#include <cstdlib>
#include <string>

EM_JS(int, js_serial_count, (void), { return Module.webSerial ? Module.webSerial.ports().length : 0; });
EM_JS(char*, js_serial_name, (int i), {
    const p = Module.webSerial ? Module.webSerial.ports()[i] : null;
    return stringToNewUTF8(p ? p.name : "");
});
EM_JS(int, js_serial_vidpid, (int i), {
    const p = Module.webSerial ? Module.webSerial.ports()[i] : null;
    return p ? ((p.vid & 0xffff) << 16) | (p.pid & 0xffff) : 0;
});
EM_JS(int, js_serial_open, (int i, int baud), { return Module.webSerial ? (Module.webSerial.open(i, baud) ? 1 : 0) : 0; });
EM_JS(void, js_serial_close, (void), { Module.webSerial && Module.webSerial.close(); });
EM_JS(int, js_serial_write, (const uint8_t* p, int n), {
    return Module.webSerial ? Module.webSerial.write(HEAPU8.slice(p, p + n)) : -1;
});

static PcUart* s_open = nullptr;

static std::string takeString(char* p) { std::string s = p ? p : ""; free(p); return s; }

std::vector<std::string> PcUart::enumeratePorts() {
    std::vector<std::string> out;
    for (int i = 0, n = js_serial_count(); i < n; ++i) out.push_back(takeString(js_serial_name(i)));
    return out;
}

std::vector<std::string> PcUart::findPortsByVidPid(uint16_t vid, uint16_t pid) {
    std::vector<std::string> out;
    for (int i = 0, n = js_serial_count(); i < n; ++i) {
        const uint32_t vp = (uint32_t)js_serial_vidpid(i);
        if ((vp >> 16) == vid && (vp & 0xffff) == pid) out.push_back(takeString(js_serial_name(i)));
    }
    return out;
}

bool PcUart::open(const std::string& portName, BaudRate baud) {
    close();
    const auto ports = enumeratePorts();
    for (size_t i = 0; i < ports.size(); ++i) {
        if (ports[i] != portName) continue;
        if (!js_serial_open((int)i, (int)baud)) return false;
        portName_ = portName;
        baudRate_ = baud;
        open_.store(true);
        s_open = this;
        printf("[UART] Opened %s @ %u\n", portName.c_str(), (unsigned)baud);
        return true;
    }
    return false;
}

void PcUart::close() {
    if (!open_.load()) return;
    js_serial_close();
    printf("[UART] Closed %s\n", portName_.c_str());
    portName_.clear();
    open_.store(false);
    if (s_open == this) s_open = nullptr;
    std::lock_guard<std::mutex> lock(bufMutex_);
    lineBuffer_.clear();
    partialLine_.clear();
}

PcUart::~PcUart() { close(); }

int PcUart::write(const uint8_t* data, size_t len) {
    if (!open_.load()) return -1;
    return js_serial_write(data, (int)len);
}

int PcUart::write(const std::string& text) {
    return write(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

void PcUart::readerLoop() {}

std::string PcUart::getPortName() const { return portName_; }

void PcUart::setOnLineReceived(LineCallback cb) {
    std::lock_guard<std::mutex> lock(bufMutex_);
    lineCb_ = std::move(cb);
}

std::vector<std::string> PcUart::readLines() {
    std::lock_guard<std::mutex> lock(bufMutex_);
    std::vector<std::string> result;
    result.swap(lineBuffer_);
    return result;
}

static uint8_t s_rxBuf[4096];

extern "C" {
EMSCRIPTEN_KEEPALIVE uint8_t* wasm_uart_buf() { return s_rxBuf; }

/** Bytes the page read from the open port (in s_rxBuf). */
EMSCRIPTEN_KEEPALIVE void wasm_uart_rx(int len) {
    PcUart* u = s_open;
    if (!u || len <= 0) return;
    u->feed(s_rxBuf, (size_t)len);
}

/** The page lost the port (unplugged). */
EMSCRIPTEN_KEEPALIVE void wasm_uart_lost() {
    if (s_open) s_open->close();
}
}

void PcUart::feed(const uint8_t* buf, size_t n) {
    std::lock_guard<std::mutex> lock(bufMutex_);
    for (size_t i = 0; i < n; i++) {
        const char c = static_cast<char>(buf[i]);
        if (c == '\n') {
            if (!partialLine_.empty() && partialLine_.back() == '\r') partialLine_.pop_back();
            if (lineBuffer_.size() >= MAX_BUFFERED_LINES) lineBuffer_.erase(lineBuffer_.begin());
            lineBuffer_.push_back(partialLine_);
            totalLines_.fetch_add(1);
            if (lineCb_) lineCb_(partialLine_);
            partialLine_.clear();
        } else {
            partialLine_ += c;
        }
    }
}
