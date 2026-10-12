#pragma once

/**
 * @file FreeRtosEventBus.hpp
 * @brief FreeRTOS-based IEventBus — async queue dispatch using raw FreeRTOS primitives.
 *
 * Runs on the simulator's FreeRTOS port (POSIX / MSVC-MingW).
 * post*() methods enqueue events for async dispatch on a dedicated task.
 * send*() methods dispatch synchronously on the calling thread.
 */

#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

#include "crosspad/event/IEventBus.hpp"
#include <vector>

namespace crosspad {

class FreeRtosEventBus : public IEventBus {
public:
    FreeRtosEventBus() = default;
    ~FreeRtosEventBus() override;

    bool init() override;

    SubscriptionHandle subscribe(EventType type, EventHandler handler, void* user_ctx = nullptr) override;
    void unsubscribe(SubscriptionHandle handle) override;

    bool postNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) override;
    bool postNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) override;
    bool postPadPressed(uint8_t padIdx, bool isOn, uint8_t velocity, EventSource source = EventSource::PadManager) override;
    bool postPadReleased(uint8_t padIdx, EventSource source = EventSource::PadManager) override;
    bool postAppChanged(const char* appName, EventSource source = EventSource::System) override;
    bool postTempoChanged(float bpm, EventSource source = EventSource::SequencerCallback) override;
    bool postLiveModeChanged(bool enabled, EventSource source = EventSource::System) override;
    bool postGuiUpdateRequired(EventSource source = EventSource::Gui) override;
    bool postSamplePlayback(uint8_t note, uint8_t wavIdx, bool isOn, EventSource source = EventSource::SamplePlayback, uint8_t velocity = 127,
                            int8_t transpose = 0, uint8_t key = kSamplePlaybackOwnKey) override;
    bool postMotionGesture(MotionGesture gesture, uint8_t orientation = 0, EventSource source = EventSource::Imu) override;

    bool sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source = EventSource::System) override;
    bool sendNoteOff(uint8_t channel, uint8_t note, EventSource source = EventSource::System) override;

private:
    static constexpr size_t ENVELOPE_DATA_SIZE = 48;
    static constexpr UBaseType_t QUEUE_LENGTH = 64;
    static constexpr UBaseType_t DISPATCH_TASK_STACK = 4096;
    static constexpr UBaseType_t DISPATCH_TASK_PRIO = 2;

    struct EventEnvelope {
        EventType type;
        uint8_t data[ENVELOPE_DATA_SIZE];
        size_t dataSize;
    };

    struct Subscription {
        EventType type;
        EventHandler handler;
        void* userCtx;
    };

    void dispatch(EventType type, const void* data, size_t dataSize);
    bool enqueue(EventType type, const void* data, size_t dataSize);

    static void dispatchTaskEntry(void* param);
    void dispatchLoop();

    std::vector<Subscription*> subs_;
    SemaphoreHandle_t mutex_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t dispatchTask_ = nullptr;
    bool initialized_ = false;
};

} // namespace crosspad
