/**
 * @file FreeRtosEventBus.cpp
 * @brief FreeRTOS-based event bus — async queue + dedicated dispatch task.
 */

#include "pc_stubs/FreeRtosEventBus.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace crosspad {

FreeRtosEventBus::~FreeRtosEventBus() {
    if (dispatchTask_) {
        vTaskDelete(dispatchTask_);
        dispatchTask_ = nullptr;
    }
    if (queue_) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
    if (mutex_) {
        xSemaphoreTake(mutex_, portMAX_DELAY);
        for (auto* sub : subs_) delete sub;
        subs_.clear();
        xSemaphoreGive(mutex_);
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
    }
}

bool FreeRtosEventBus::init() {
    if (initialized_) return true;

    mutex_ = xSemaphoreCreateMutex();
    if (!mutex_) {
        printf("[EventBus] Failed to create mutex\n");
        return false;
    }

    queue_ = xQueueCreate(QUEUE_LENGTH, sizeof(EventEnvelope));
    if (!queue_) {
        printf("[EventBus] Failed to create queue\n");
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
        return false;
    }

    BaseType_t ret = xTaskCreate(
        dispatchTaskEntry, "EvtBus",
        DISPATCH_TASK_STACK, this,
        DISPATCH_TASK_PRIO, &dispatchTask_);

    if (ret != pdPASS) {
        printf("[EventBus] Failed to create dispatch task\n");
        vQueueDelete(queue_);
        queue_ = nullptr;
        vSemaphoreDelete(mutex_);
        mutex_ = nullptr;
        return false;
    }

    initialized_ = true;
    printf("[EventBus] FreeRTOS EventBus initialized (queue=%u, prio=%u)\n",
           (unsigned)QUEUE_LENGTH, (unsigned)DISPATCH_TASK_PRIO);
    return true;
}

SubscriptionHandle FreeRtosEventBus::subscribe(EventType type, EventHandler handler, void* user_ctx) {
    if (!initialized_) {
        if (!init()) return nullptr;
    }

    auto* sub = new Subscription{type, handler, user_ctx};
    xSemaphoreTake(mutex_, portMAX_DELAY);
    subs_.push_back(sub);
    xSemaphoreGive(mutex_);
    return static_cast<SubscriptionHandle>(sub);
}

void FreeRtosEventBus::unsubscribe(SubscriptionHandle handle) {
    if (!handle) return;

    auto* sub = static_cast<Subscription*>(handle);
    xSemaphoreTake(mutex_, portMAX_DELAY);
    subs_.erase(std::remove(subs_.begin(), subs_.end(), sub), subs_.end());
    xSemaphoreGive(mutex_);
    delete sub;
}

// ── Direct dispatch (synchronous, under mutex) ──────────────────────────────

void FreeRtosEventBus::dispatch(EventType type, const void* data, size_t dataSize) {
    (void)dataSize;

    // Copy subs under mutex, then iterate outside to avoid holding lock during callbacks
    xSemaphoreTake(mutex_, portMAX_DELAY);
    std::vector<Subscription*> snapshot(subs_);
    xSemaphoreGive(mutex_);

    for (auto* sub : snapshot) {
        if (sub->type == type) {
            sub->handler(data, type, sub->userCtx);
        }
    }
}

// ── Queue-based async post ──────────────────────────────────────────────────

bool FreeRtosEventBus::enqueue(EventType type, const void* data, size_t dataSize) {
    if (!queue_) return false;

    EventEnvelope env;
    env.type = type;
    env.dataSize = (dataSize <= ENVELOPE_DATA_SIZE) ? dataSize : ENVELOPE_DATA_SIZE;
    memcpy(env.data, data, env.dataSize);

    BaseType_t ret = xQueueSend(queue_, &env, pdMS_TO_TICKS(100));
    if (ret != pdPASS) {
        printf("[EventBus] Queue full, dropped %s event\n", getEventName(type));
        return false;
    }
    return true;
}

// ── Dispatch task ───────────────────────────────────────────────────────────

void FreeRtosEventBus::dispatchTaskEntry(void* param) {
    static_cast<FreeRtosEventBus*>(param)->dispatchLoop();
}

void FreeRtosEventBus::dispatchLoop() {
    EventEnvelope env;
    for (;;) {
        if (xQueueReceive(queue_, &env, portMAX_DELAY) == pdPASS) {
            dispatch(env.type, env.data, env.dataSize);
        }
    }
}

// ── Async post methods (queued) ─────────────────────────────────────────────

bool FreeRtosEventBus::postNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) {
    NoteOnData data{channel, note, velocity, source};
    return enqueue(EventType::NoteOn, &data, sizeof(data));
}

bool FreeRtosEventBus::postNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) {
    NoteOffData data{channel, note, velocity, source};
    return enqueue(EventType::NoteOff, &data, sizeof(data));
}

bool FreeRtosEventBus::postPadPressed(uint8_t padIdx, bool isOn, uint8_t velocity, EventSource source) {
    PadPressedData data{padIdx, isOn, velocity, source};
    return enqueue(EventType::PadPressed, &data, sizeof(data));
}

bool FreeRtosEventBus::postPadReleased(uint8_t padIdx, EventSource source) {
    PadReleasedData data{padIdx, source};
    return enqueue(EventType::PadReleased, &data, sizeof(data));
}

bool FreeRtosEventBus::postAppChanged(const char* appName, EventSource source) {
    AppChangedData data;
    data.setName(appName);
    data.source = source;
    return enqueue(EventType::AppChanged, &data, sizeof(data));
}

bool FreeRtosEventBus::postTempoChanged(float bpm, EventSource source) {
    TempoChangedData data{bpm, source};
    return enqueue(EventType::TempoChanged, &data, sizeof(data));
}

bool FreeRtosEventBus::postLiveModeChanged(bool enabled, EventSource source) {
    LiveModeChangedData data{enabled, source};
    return enqueue(EventType::LiveModeChanged, &data, sizeof(data));
}

bool FreeRtosEventBus::postGuiUpdateRequired(EventSource source) {
    return enqueue(EventType::GuiUpdateRequired, &source, sizeof(source));
}

bool FreeRtosEventBus::postSamplePlayback(uint8_t note, uint8_t wavIdx, bool isOn, EventSource source, uint8_t velocity,
                                          int8_t transpose, uint8_t key) {
    SamplePlaybackData data{note, wavIdx, isOn, velocity, source, transpose, key};
    return enqueue(EventType::SamplePlayback, &data, sizeof(data));
}

bool FreeRtosEventBus::postMotionGesture(MotionGesture gesture, uint8_t orientation, EventSource source) {
    MotionGestureData data{gesture, orientation, source};
    return enqueue(EventType::MotionGesture, &data, sizeof(data));
}

// ── Sync send methods (direct dispatch, same thread) ────────────────────────

bool FreeRtosEventBus::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) {
    NoteOnData data{channel, note, velocity, source};
    dispatch(EventType::NoteOn, &data, sizeof(data));
    return true;
}

bool FreeRtosEventBus::sendNoteOff(uint8_t channel, uint8_t note, EventSource source) {
    NoteOffData data{channel, note, 0, source};
    dispatch(EventType::NoteOff, &data, sizeof(data));
    return true;
}

} // namespace crosspad
