// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file WavReader.hpp
 * @brief Minimal RIFF/WAVE reader for the PC sample engine.
 *
 * Deliberately narrow: the engine plays 16-bit PCM only, at whatever sample
 * rate the file carries (no resampling — same as the embedded player, where a
 * 44.1 kHz file in a 48 kHz engine simply sounds sharp). Anything else is
 * reported by wav_read_info() but refused by WavReader::open(), so a bad file
 * is a silent pad with a log line rather than noise.
 *
 * Reads are framed, not byte-oriented: readStereo() always emits interleaved
 * stereo, duplicating mono and dropping channels above the first two, so
 * everything downstream of this class handles exactly one layout.
 */

#include <cstdint>
#include <cstdio>

namespace crosspad_pc {

struct WavInfo {
    uint32_t sampleRate    = 0;
    uint16_t channels      = 0;
    uint16_t bitsPerSample = 0;
    uint16_t blockAlign    = 0;   ///< bytes per frame in the file
    uint32_t dataOffset    = 0;   ///< byte offset of the first sample
    uint32_t dataBytes     = 0;
    uint32_t frameCount    = 0;   ///< dataBytes / blockAlign
};

/// Parse the header only. Succeeds for any PCM layout the chunk walker
/// understands, including ones open() will refuse.
bool wav_read_info(const char* path, WavInfo& out);

/**
 * @brief An open 16-bit PCM WAV positioned at a frame.
 *
 * One reader per voice: the embedded player shares a single global FILE* and
 * pays for it with a mutex, a render gate and two documented panics. On PC a
 * file handle is cheap, so each voice owns one and the streaming path needs no
 * arbitration at all.
 */
class WavReader {
public:
    WavReader() = default;
    ~WavReader();

    WavReader(const WavReader&)            = delete;
    WavReader& operator=(const WavReader&) = delete;

    /// Open and validate. Fails on anything but 16-bit PCM with 1..N channels.
    bool open(const char* path);
    void close();
    bool isOpen() const { return f_ != nullptr; }

    const WavInfo& info() const { return info_; }

    /// Seek to a frame index counted from the start of the data chunk.
    bool seekFrame(uint32_t frame);

    /// Read up to `frames` frames as interleaved stereo int16 (2 values per
    /// frame). Returns the number of frames actually produced.
    uint32_t readStereo(int16_t* dst, uint32_t frames);

private:
    FILE*   f_       = nullptr;
    WavInfo info_{};
    uint32_t nextFrame_ = 0;
};

} // namespace crosspad_pc
