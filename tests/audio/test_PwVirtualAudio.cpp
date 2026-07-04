// Integration tests for the native PipeWire virtual audio backend.
// Skipped gracefully when no PipeWire daemon is reachable (CI, containers).
#if defined(__linux__) && defined(USE_PIPEWIRE)
#include <catch2/catch_test_macros.hpp>
#include "audio/pipewire/PwContext.hpp"

static bool pwTestAvailable() {
    return crosspad_pc::PwContext::instance().init();
}

TEST_CASE("PwContext connects to the daemon or degrades cleanly", "[pipewire]") {
    if (!pwTestAvailable()) { SUCCEED("no PipeWire daemon"); return; }
    REQUIRE(crosspad_pc::PwContext::instance().isConnected());
}

TEST_CASE("PwContext is re-initializable after a clean shutdown", "[pipewire]") {
    if (!pwTestAvailable()) { SUCCEED("no PipeWire daemon"); return; }
    auto& ctx = crosspad_pc::PwContext::instance();

    ctx.shutdown();
    REQUIRE_FALSE(ctx.isConnected());
    ctx.shutdown();  // safe to call twice
    REQUIRE_FALSE(ctx.isConnected());

    // A clean shutdown must not permanently block reconnection.
    REQUIRE(ctx.init());
    REQUIRE(ctx.isConnected());
    REQUIRE(ctx.core() != nullptr);
    REQUIRE(ctx.loop() != nullptr);

    // Lock is a no-op (not a crash) on a shut-down context.
    ctx.shutdown();
    { crosspad_pc::PwContext::Lock lock(ctx); }
    REQUIRE(ctx.init());  // leave connected for any later [pipewire] tests
}
#endif
