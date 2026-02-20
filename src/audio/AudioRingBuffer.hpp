#pragma once

#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

/**
 * @brief Lock-free Single-Producer Single-Consumer (SPSC) ring buffer.
 *
 * Used to decouple the audio write() call (producer, main/synth thread)
 * from the RtAudio real-time callback (consumer, audio thread).
 */
template <typename T>
class AudioRingBuffer {
public:
    explicit AudioRingBuffer(size_t capacity = 0)
        : buffer_(capacity), capacity_(capacity), head_(0), tail_(0) {}

    void resize(size_t capacity) {
        buffer_.resize(capacity);
        capacity_ = capacity;
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    size_t write(const T* data, size_t count) {
        if (capacity_ == 0) return 0;

        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_acquire);

        size_t avail = capacity_ - 1 - ((h - t + capacity_) % capacity_);
        if (count > avail) count = avail;
        if (count == 0) return 0;

        const size_t pos = h % capacity_;
        const size_t first = (pos + count <= capacity_) ? count : (capacity_ - pos);
        std::memcpy(buffer_.data() + pos, data, first * sizeof(T));
        if (first < count) {
            std::memcpy(buffer_.data(), data + first, (count - first) * sizeof(T));
        }

        head_.store(h + count, std::memory_order_release);
        return count;
    }

    size_t read(T* data, size_t count) {
        if (capacity_ == 0) return 0;

        const size_t t = tail_.load(std::memory_order_relaxed);
        const size_t h = head_.load(std::memory_order_acquire);

        size_t avail = (h - t + capacity_) % capacity_;
        if (count > avail) count = avail;
        if (count == 0) return 0;

        const size_t pos = t % capacity_;
        const size_t first = (pos + count <= capacity_) ? count : (capacity_ - pos);
        std::memcpy(data, buffer_.data() + pos, first * sizeof(T));
        if (first < count) {
            std::memcpy(data, buffer_.data() + first, (count - first) * sizeof(T));
        }

        tail_.store(t + count, std::memory_order_release);
        return count;
    }

    size_t available() const {
        const size_t h = head_.load(std::memory_order_acquire);
        const size_t t = tail_.load(std::memory_order_acquire);
        return (h - t + capacity_) % capacity_;
    }

    size_t space() const {
        return capacity_ - 1 - available();
    }

    void reset() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<T> buffer_;
    size_t capacity_;
    std::atomic<size_t> head_;
    std::atomic<size_t> tail_;
};
