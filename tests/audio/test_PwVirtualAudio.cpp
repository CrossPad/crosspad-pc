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
#endif
