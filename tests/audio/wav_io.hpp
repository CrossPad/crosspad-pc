// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file wav_io.hpp
 * @brief Minimal WAV reader/writer for golden audio comparisons.
 *
 * Only supports PCM int16 stereo. Header-only.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace crosspad_test {

struct WavFile {
    uint32_t sampleRate = 48000;
    uint16_t channels   = 2;
    std::vector<int16_t> samples;   // interleaved
};

inline bool wavWrite(const std::string& path, const WavFile& w) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;

    const uint32_t dataBytes = static_cast<uint32_t>(w.samples.size() * sizeof(int16_t));
    const uint32_t fmtSize   = 16;
    const uint16_t bitsPer   = 16;
    const uint16_t blockAlign= w.channels * (bitsPer / 8);
    const uint32_t byteRate  = w.sampleRate * blockAlign;
    const uint32_t riffSize  = 36 + dataBytes;
    const uint16_t pcmFmt    = 1;

    auto write32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto write16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };

    f.write("RIFF", 4);
    write32(riffSize);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    write32(fmtSize);
    write16(pcmFmt);
    write16(w.channels);
    write32(w.sampleRate);
    write32(byteRate);
    write16(blockAlign);
    write16(bitsPer);
    f.write("data", 4);
    write32(dataBytes);
    f.write(reinterpret_cast<const char*>(w.samples.data()), dataBytes);

    return f.good();
}

inline bool wavRead(const std::string& path, WavFile& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    char tag[5] = {0};
    auto read32 = [&]() { uint32_t v; f.read(reinterpret_cast<char*>(&v), 4); return v; };
    auto read16 = [&]() { uint16_t v; f.read(reinterpret_cast<char*>(&v), 2); return v; };

    f.read(tag, 4); if (std::memcmp(tag, "RIFF", 4) != 0) return false;
    read32();   // riff size
    f.read(tag, 4); if (std::memcmp(tag, "WAVE", 4) != 0) return false;

    // Skip chunks until we find fmt + data
    bool gotFmt = false, gotData = false;
    uint32_t dataBytes = 0;
    while (f && !(gotFmt && gotData)) {
        f.read(tag, 4);
        if (!f) break;
        const uint32_t chunkSize = read32();
        if (std::memcmp(tag, "fmt ", 4) == 0) {
            const uint16_t fmt = read16();
            if (fmt != 1) return false;     // PCM only
            out.channels   = read16();
            out.sampleRate = read32();
            read32();   // byte rate
            read16();   // block align
            const uint16_t bits = read16();
            if (bits != 16) return false;
            const uint32_t consumed = 16;
            if (chunkSize > consumed) f.seekg(chunkSize - consumed, std::ios::cur);
            gotFmt = true;
        } else if (std::memcmp(tag, "data", 4) == 0) {
            dataBytes = chunkSize;
            out.samples.resize(dataBytes / 2);
            f.read(reinterpret_cast<char*>(out.samples.data()), dataBytes);
            gotData = true;
        } else {
            f.seekg(chunkSize, std::ios::cur);
        }
    }
    return gotFmt && gotData;
}

/// Per-sample max-abs diff. Suitable for deterministic mixers; for synths
/// with floating-point noise consider an RMS-based comparison instead.
inline int wavMaxAbsDiff(const std::vector<int16_t>& a, const std::vector<int16_t>& b) {
    const size_t n = std::min(a.size(), b.size());
    int worst = 0;
    for (size_t i = 0; i < n; ++i) {
        const int d = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        if (d > worst) worst = d;
    }
    return worst;
}

inline double wavRmsDiffRatio(const std::vector<int16_t>& a, const std::vector<int16_t>& b) {
    const size_t n = std::min(a.size(), b.size());
    if (n == 0) return 0.0;
    double sumDiff = 0.0;
    double sumRef  = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        sumDiff += d * d;
        sumRef  += static_cast<double>(a[i]) * a[i];
    }
    if (sumRef == 0.0) return sumDiff > 0.0 ? 1e30 : 0.0;
    return std::sqrt(sumDiff / sumRef);
}

} // namespace crosspad_test
