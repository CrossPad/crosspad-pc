#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include "crosspad/event/FreeRtosEventBus.hpp"
#include "crosspad/event/EventTypes.hpp"
#include "crosspad/event/EventData.hpp"

#include "FreeRTOS.h"
#include "task.h"

#include <atomic>

using namespace crosspad;

TEST_CASE("FreeRtosEventBus: init and subscribe", "[eventbus]") {
    FreeRtosEventBus bus;
    REQUIRE(bus.init());

    SECTION("subscribe returns valid handle") {
        auto handle = bus.subscribe(EventType::NoteOn,
            [](const void*, EventType, void*) {}, nullptr);
        REQUIRE(handle != nullptr);
        bus.unsubscribe(handle);
    }
}

TEST_CASE("FreeRtosEventBus: sync send delivers immediately", "[eventbus]") {
    FreeRtosEventBus bus;
    REQUIRE(bus.init());

    std::atomic<int> callCount{0};
    std::atomic<uint8_t> receivedNote{0};

    auto handle = bus.subscribe(EventType::NoteOn,
        [](const void* data, EventType type, void* ctx) {
            auto* note = static_cast<const NoteOnData*>(data);
            auto* count = static_cast<std::atomic<int>*>(ctx);
            count->fetch_add(1);
        }, &callCount);

    // sendNoteOn dispatches synchronously
    bus.sendNoteOn(0, 60, 127);

    REQUIRE(callCount.load() == 1);

    bus.unsubscribe(handle);
}

TEST_CASE("FreeRtosEventBus: async post delivers via queue", "[eventbus]") {
    FreeRtosEventBus bus;
    REQUIRE(bus.init());

    std::atomic<int> callCount{0};

    auto handle = bus.subscribe(EventType::PadPressed,
        [](const void* data, EventType type, void* ctx) {
            auto* count = static_cast<std::atomic<int>*>(ctx);
            count->fetch_add(1);
        }, &callCount);

    // postPadPressed is async — queued for dispatch task
    bus.postPadPressed(0, true, 127);

    // Give the dispatch task time to process
    vTaskDelay(pdMS_TO_TICKS(50));

    REQUIRE(callCount.load() == 1);

    bus.unsubscribe(handle);
}

TEST_CASE("FreeRtosEventBus: unsubscribe stops delivery", "[eventbus]") {
    FreeRtosEventBus bus;
    REQUIRE(bus.init());

    std::atomic<int> callCount{0};

    auto handle = bus.subscribe(EventType::NoteOn,
        [](const void* data, EventType type, void* ctx) {
            auto* count = static_cast<std::atomic<int>*>(ctx);
            count->fetch_add(1);
        }, &callCount);

    bus.sendNoteOn(0, 60, 127);
    REQUIRE(callCount.load() == 1);

    bus.unsubscribe(handle);

    bus.sendNoteOn(0, 61, 127);
    REQUIRE(callCount.load() == 1); // no increase
}
