/**
 * @file PcEventBus.cpp
 * @brief Desktop event bus — synchronous direct dispatch
 */

#include "PcEventBus.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>

namespace crosspad {

bool PcEventBus::init() {
    if (initialized_) return true;
    initialized_ = true;
    printf("[EventBus] PC EventBus initialized (direct dispatch)\n");
    return true;
}

SubscriptionHandle PcEventBus::subscribe(EventType type, EventHandler handler, void* user_ctx) {
    if (!initialized_) {
        if (!init()) return nullptr;
    }

    auto* sub = new Subscription{type, handler, user_ctx};
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.push_back(sub);
    return static_cast<SubscriptionHandle>(sub);
}

void PcEventBus::unsubscribe(SubscriptionHandle handle) {
    if (!handle) return;

    auto* sub = static_cast<Subscription*>(handle);
    std::lock_guard<std::mutex> lock(mutex_);
    subs_.erase(std::remove(subs_.begin(), subs_.end(), sub), subs_.end());
    delete sub;
}

void PcEventBus::dispatch(EventType type, const void* data, size_t dataSize) {
    (void)dataSize;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto* sub : subs_) {
        if (sub->type == type) {
            sub->handler(data, type, sub->userCtx);
        }
    }
}

// ── Async post methods ──────────────────────────────────────────────────────

bool PcEventBus::postNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) {
    NoteOnData data{channel, note, velocity, source};
    dispatch(EventType::NoteOn, &data, sizeof(data));
    return true;
}

bool PcEventBus::postNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) {
    NoteOffData data{channel, note, velocity, source};
    dispatch(EventType::NoteOff, &data, sizeof(data));
    return true;
}

bool PcEventBus::postPadPressed(uint8_t padIdx, bool isOn, uint8_t velocity, EventSource source) {
    PadPressedData data{padIdx, isOn, velocity, source};
    dispatch(EventType::PadPressed, &data, sizeof(data));
    return true;
}

bool PcEventBus::postPadReleased(uint8_t padIdx, EventSource source) {
    PadReleasedData data{padIdx, source};
    dispatch(EventType::PadReleased, &data, sizeof(data));
    return true;
}

bool PcEventBus::postAppChanged(const char* appName, EventSource source) {
    AppChangedData data;
    data.setName(appName);
    data.source = source;
    dispatch(EventType::AppChanged, &data, sizeof(data));
    return true;
}

bool PcEventBus::postTempoChanged(float bpm, EventSource source) {
    TempoChangedData data{bpm, source};
    dispatch(EventType::TempoChanged, &data, sizeof(data));
    return true;
}

bool PcEventBus::postLiveModeChanged(bool enabled, EventSource source) {
    LiveModeChangedData data{enabled, source};
    dispatch(EventType::LiveModeChanged, &data, sizeof(data));
    return true;
}

bool PcEventBus::postGuiUpdateRequired(EventSource source) {
    dispatch(EventType::GuiUpdateRequired, &source, sizeof(source));
    return true;
}

bool PcEventBus::postSamplePlayback(uint8_t note, uint8_t wavIdx, bool isOn, EventSource source) {
    SamplePlaybackData data{note, wavIdx, isOn, source};
    dispatch(EventType::SamplePlayback, &data, sizeof(data));
    return true;
}

// ── Sync send methods ───────────────────────────────────────────────────────

bool PcEventBus::sendNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, EventSource source) {
    NoteOnData data{channel, note, velocity, source};
    dispatch(EventType::NoteOn, &data, sizeof(data));
    return true;
}

bool PcEventBus::sendNoteOff(uint8_t channel, uint8_t note, EventSource source) {
    NoteOffData data{channel, note, 0, source};
    dispatch(EventType::NoteOff, &data, sizeof(data));
    return true;
}

} // namespace crosspad
