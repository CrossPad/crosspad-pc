#include <catch2/catch_test_macros.hpp>
#include "test_helpers.hpp"
#include "pc_stubs/FreeRtosEventBus.hpp"
#include "crosspad/event/EventTypes.hpp"
#include "crosspad/event/EventData.hpp"

#include "FreeRTOS.h"
#include "semphr.h"
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

// ── Subscription contract (IEventBus::unsubscribe) ─────────────────────────

namespace {

struct BlockingHandlerCtx {
    SemaphoreHandle_t entered = xSemaphoreCreateBinary();
    SemaphoreHandle_t release = xSemaphoreCreateBinary();
    SemaphoreHandle_t unsubscribeReturned = xSemaphoreCreateBinary();
    FreeRtosEventBus* bus = nullptr;
    SubscriptionHandle blocked = nullptr;
    std::atomic<int> calls{0};
    std::atomic<bool> unsubscribed{false};
    std::atomic<bool> unsubscribedWhileRunning{false};

    ~BlockingHandlerCtx() {
        vSemaphoreDelete(entered);
        vSemaphoreDelete(release);
        vSemaphoreDelete(unsubscribeReturned);
    }
};

void blockingHandler(const void*, EventType, void* user) {
    auto* c = static_cast<BlockingHandlerCtx*>(user);
    c->calls++;
    xSemaphoreGive(c->entered);
    xSemaphoreTake(c->release, portMAX_DELAY);
    c->unsubscribedWhileRunning = c->unsubscribed.load();
}

void unsubscriberTask(void* param) {
    auto* c = static_cast<BlockingHandlerCtx*>(param);
    c->bus->unsubscribe(c->blocked);
    c->unsubscribed = true;
    xSemaphoreGive(c->unsubscribeReturned);
    vTaskDelete(nullptr);
}

struct Reentrant {
    FreeRtosEventBus* bus = nullptr;
    SubscriptionHandle other = nullptr;
    SubscriptionHandle added = nullptr;
    int otherCalls = 0;
    int addedCalls = 0;
};

} // namespace

TEST_CASE("FreeRtosEventBus: unsubscribe from another task waits for the running handler", "[eventbus]") {
    FreeRtosEventBus bus;
    REQUIRE(bus.init());

    BlockingHandlerCtx ctx;
    ctx.bus = &bus;
    ctx.blocked = bus.subscribe(EventType::NoteOn, blockingHandler, &ctx);

    bus.postNoteOn(0, 60, 100, EventSource::System);
    REQUIRE(xSemaphoreTake(ctx.entered, portMAX_DELAY) == pdTRUE);

    REQUIRE(xTaskCreate(unsubscriberTask, "unsub", 4096, &ctx, 1, nullptr) == pdPASS);

    // A broken bus returns at once; a correct one cannot return before the
    // release below, so the bounded wait only gives a broken one its chance.
    const bool returnedEarly = xSemaphoreTake(ctx.unsubscribeReturned, pdMS_TO_TICKS(50)) == pdTRUE;
    CHECK_FALSE(returnedEarly);
    xSemaphoreGive(ctx.release);
    if (!returnedEarly) xSemaphoreTake(ctx.unsubscribeReturned, portMAX_DELAY);

    CHECK_FALSE(ctx.unsubscribedWhileRunning.load());
    bus.sendNoteOn(0, 61, 100);
    CHECK(ctx.calls.load() == 1);
}

TEST_CASE("FreeRtosEventBus: a handler unsubscribes another and subscribes a new one", "[eventbus]") {
    FreeRtosEventBus bus;
    REQUIRE(bus.init());

    Reentrant r;
    r.bus = &bus;
    auto first = bus.subscribe(EventType::NoteOn,
        [](const void*, EventType, void* user) {
            auto* c = static_cast<Reentrant*>(user);
            if (c->added) return;
            c->bus->unsubscribe(c->other);
            c->added = c->bus->subscribe(EventType::NoteOn,
                [](const void*, EventType, void* u) { static_cast<Reentrant*>(u)->addedCalls++; }, c);
        }, &r);
    r.other = bus.subscribe(EventType::NoteOn,
        [](const void*, EventType, void* user) { static_cast<Reentrant*>(user)->otherCalls++; }, &r);

    bus.sendNoteOn(0, 60, 100);
    CHECK(r.otherCalls == 0);
    CHECK(r.addedCalls == 0);

    bus.sendNoteOn(0, 61, 100);
    CHECK(r.otherCalls == 0);
    CHECK(r.addedCalls == 1);

    bus.unsubscribe(first);
    bus.unsubscribe(r.added);
}

TEST_CASE("FreeRtosEventBus: unsubscribe from inside a handler does not wait for another task", "[eventbus]") {
    FreeRtosEventBus bus;
    REQUIRE(bus.init());

    BlockingHandlerCtx ctx;
    ctx.bus = &bus;
    ctx.blocked = bus.subscribe(EventType::NoteOn, blockingHandler, &ctx);
    auto remover = bus.subscribe(EventType::NoteOff,
        [](const void*, EventType, void* user) {
            auto* c = static_cast<BlockingHandlerCtx*>(user);
            c->bus->unsubscribe(c->blocked);
        }, &ctx);

    bus.postNoteOn(0, 60, 100, EventSource::System);
    REQUIRE(xSemaphoreTake(ctx.entered, portMAX_DELAY) == pdTRUE);

    bus.sendNoteOff(0, 60);
    xSemaphoreGive(ctx.release);

    bus.sendNoteOn(0, 61, 100);
    CHECK(ctx.calls.load() == 1);
    bus.unsubscribe(remover);
}
