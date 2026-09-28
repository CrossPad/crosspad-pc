// SPDX-License-Identifier: MIT
#pragma once
// PC counterpart of platform-idf's AudioFileIo.hpp for the sample tools: the
// same FrameRing and ClipReader, on the host heap and the host filesystem.
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>

std::string pc_platform_get_sdcard_path();

namespace crosspad {

inline void give_way() { std::this_thread::yield(); }
inline bool card_can_open() { return !pc_platform_get_sdcard_path().empty(); }

/// Stereo int16 frames, one writer and one reader, counters that only grow.
struct FrameRing {
    int16_t* buf = nullptr;
    uint32_t cap = 0;
    std::atomic<uint32_t> head{0}, tail{0};

    bool alloc(uint32_t frames, uint32_t minFrames) {
        release();
        buf = static_cast<int16_t*>(std::calloc(frames, 4));
        cap = buf ? frames : 0;
        return cap >= minFrames;
    }
    void release() { std::free(buf); buf = nullptr; cap = 0; head.store(0); tail.store(0); }
    uint32_t used()  const { return head.load(std::memory_order_acquire) - tail.load(std::memory_order_acquire); }
    uint32_t space() const { return cap - used(); }
    void copy(uint32_t at, int16_t* ext, uint32_t n, bool toRing) {
        while (n) {
            const uint32_t pos = at % cap, run = std::min(n, cap - pos);
            int16_t* p = buf + pos * 2;
            if (toRing) { if (ext) std::memcpy(p, ext, run * 4); else std::memset(p, 0, run * 4); }
            else        std::memcpy(ext, p, run * 4);
            if (ext) ext += run * 2;
            at += run; n -= run;
        }
    }
    uint32_t push(const int16_t* src, uint32_t n) {
        if (!cap) return 0;
        n = std::min(n, space());
        const uint32_t h = head.load(std::memory_order_relaxed);
        copy(h, const_cast<int16_t*>(src), n, true);
        head.store(h + n, std::memory_order_release);
        return n;
    }
    uint32_t pop(int16_t* dst, uint32_t n) {
        if (!cap) return 0;
        n = std::min(n, used());
        const uint32_t t = tail.load(std::memory_order_relaxed);
        copy(t, dst, n, false);
        tail.store(t + n, std::memory_order_release);
        return n;
    }
};

/// A 16-bit PCM WAV being read, mono or stereo (mono is played to both sides).
struct ClipReader {
    int         fd = -1;
    std::string path;
    uint32_t    dataOffset = 0;
    uint32_t    frames = 0;
    uint16_t    channels = 2;
    uint32_t    cursor = UINT32_MAX;

    void close() { if (fd >= 0) ::close(fd); fd = -1; path.clear(); cursor = UINT32_MAX; }
    bool readAll(void* dst, size_t n) { return ::read(fd, dst, n) == (ssize_t)n; }
    bool open(const std::string& p) {
        close();
        fd = ::open(p.c_str(), O_RDONLY);
        if (fd < 0) return false;
        uint8_t h[12];
        if (!readAll(h, 12) || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4)) { close(); return false; }
        bool fmtOk = false;
        uint32_t at = 12;
        for (;;) {
            uint8_t ch[8];
            if (!readAll(ch, 8)) break;
            at += 8;
            const uint32_t size = ch[4] | ch[5] << 8 | ch[6] << 16 | (uint32_t)ch[7] << 24;
            if (!std::memcmp(ch, "fmt ", 4)) {
                uint8_t fm[16];
                if (size < 16 || !readAll(fm, 16)) break;
                channels = fm[2] | fm[3] << 8;
                const uint16_t bits = fm[14] | fm[15] << 8;
                fmtOk = (fm[0] | fm[1] << 8) == 1 && bits == 16 && (channels == 1 || channels == 2);
                at += size + (size & 1);
                if (size > 16 && ::lseek(fd, (off_t)at, SEEK_SET) < 0) break;
            } else if (!std::memcmp(ch, "data", 4)) {
                if (!fmtOk) break;
                dataOffset = at; frames = size / (channels * 2u); path = p; cursor = 0;
                return true;
            } else {
                at += size + (size & 1);
                if (::lseek(fd, (off_t)at, SEEK_SET) < 0) break;
            }
        }
        close();
        return false;
    }
    uint32_t read(uint32_t frame, int16_t* dst, uint32_t n) {
        uint32_t got = 0;
        if (fd >= 0 && frame < frames) {
            if (cursor != frame) { ::lseek(fd, (off_t)(dataOffset + frame * channels * 2u), SEEK_SET); cursor = frame; }
            const uint32_t want = std::min(n, frames - frame), fb = channels * 2u;
            const ssize_t r = ::read(fd, dst, want * fb);
            got = r > 0 ? (uint32_t)r / fb : 0;
            if (channels == 1) for (int32_t i = (int32_t)got - 1; i >= 0; --i) dst[i * 2] = dst[i * 2 + 1] = dst[i];
            cursor = r > 0 && (uint32_t)r == got * fb ? cursor + got : UINT32_MAX;
        }
        if (got < n) std::memset(dst + got * 2, 0, (n - got) * 4);
        return n;
    }
};

} // namespace crosspad
