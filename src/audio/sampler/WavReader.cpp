// SPDX-License-Identifier: GPL-3.0-or-later

#include "WavReader.hpp"

#include <crosspad/instrument/WavPcm.hpp>

namespace crosspad_pc {

namespace {

// The chunk walk is core's (LIST chunks, odd-size padding, chunks in any
// order, every declared size clamped to the file); what is decided here is
// which of the files it understands the engine takes.
bool parse_header(FILE* f, WavInfo& out) {
    crosspad::WavLayout l;
    if (!crosspad::wavReadLayout(f, l, /*clampToFile=*/true)) return false;

    // WAVE_FORMAT_EXTENSIBLE (0xFFFE) carries the real tag in its subformat
    // GUID; for our purposes the sample layout in the common fields is
    // already what matters, so it is accepted like PCM.
    if (l.format != 1 && l.format != 0xFFFE) return false;
    out.channels      = l.channels;
    out.sampleRate    = l.rate;
    out.blockAlign    = l.blockAlign;
    out.bitsPerSample = l.bits;
    out.dataOffset    = l.dataOffset;
    out.dataBytes     = l.dataBytes;

    // A blockAlign of zero is legal in the struct and fatal in every consumer
    // of it — derive one rather than divide by it.
    if (out.blockAlign == 0) {
        out.blockAlign = static_cast<uint16_t>(out.channels * (out.bitsPerSample / 8));
    }
    if (out.blockAlign == 0) return false;

    out.frameCount = out.dataBytes / out.blockAlign;
    return out.frameCount > 0;
}

} // namespace

bool wav_read_info(const char* path, WavInfo& out) {
    if (!path || !path[0]) return false;
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    WavInfo info{};
    const bool ok = parse_header(f, info);
    fclose(f);
    if (ok) out = info;
    return ok;
}

WavReader::~WavReader() { close(); }

bool WavReader::open(const char* path) {
    close();
    if (!path || !path[0]) return false;
    f_ = fopen(path, "rb");
    if (!f_) return false;
    // 16-bit PCM only, and with the frame stride the interleaved read below
    // assumes: a padded blockAlign would shift every channel after the first.
    if (!parse_header(f_, info_) || info_.bitsPerSample != 16 ||
        info_.channels == 0 || info_.channels > 8 ||
        info_.blockAlign != info_.channels * 2) {
        close();
        return false;
    }
    nextFrame_ = 0;
    return fseek(f_, static_cast<long>(info_.dataOffset), SEEK_SET) == 0;
}

void WavReader::close() {
    if (f_) { fclose(f_); f_ = nullptr; }
    info_      = WavInfo{};
    nextFrame_ = 0;
}

bool WavReader::seekFrame(uint32_t frame) {
    if (!f_) return false;
    if (frame > info_.frameCount) frame = info_.frameCount;
    const long off = static_cast<long>(info_.dataOffset) +
                     static_cast<long>(frame) * static_cast<long>(info_.blockAlign);
    if (fseek(f_, off, SEEK_SET) != 0) return false;
    nextFrame_ = frame;
    return true;
}

uint32_t WavReader::readStereo(int16_t* dst, uint32_t frames) {
    if (!f_ || !dst || frames == 0) return 0;
    if (nextFrame_ >= info_.frameCount) return 0;
    if (frames > info_.frameCount - nextFrame_) frames = info_.frameCount - nextFrame_;

    // Read in chunks so a long request never needs a big scratch buffer.
    constexpr uint32_t kChunkFrames = 512;
    int16_t raw[kChunkFrames * 8];   // up to 8 channels of 16-bit
    const uint32_t chFile   = info_.channels;
    const uint32_t perChunk = (chFile <= 8) ? kChunkFrames : 0;
    if (perChunk == 0) return 0;     // refuse exotic channel counts outright

    uint32_t done = 0;
    while (done < frames) {
        const uint32_t want = (frames - done < perChunk) ? (frames - done) : perChunk;
        const size_t got = fread(raw, info_.blockAlign, want, f_);
        if (got == 0) break;
        for (size_t i = 0; i < got; ++i) {
            const int16_t l = raw[i * chFile];
            const int16_t r = (chFile > 1) ? raw[i * chFile + 1] : l;
            dst[(done + i) * 2]     = l;
            dst[(done + i) * 2 + 1] = r;
        }
        done       += static_cast<uint32_t>(got);
        nextFrame_ += static_cast<uint32_t>(got);
        if (got < want) break;
    }
    return done;
}

} // namespace crosspad_pc
