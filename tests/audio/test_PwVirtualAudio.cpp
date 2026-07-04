// Integration tests for the native PipeWire virtual audio backend.
// Skipped gracefully when no PipeWire daemon is reachable (CI, containers).
#if defined(__linux__) && defined(USE_PIPEWIRE)
#include <catch2/catch_test_macros.hpp>
#include "audio/pipewire/PwContext.hpp"
#include "audio/pipewire/PwVirtualSinkCapture.hpp"
#include "audio/pipewire/PwVirtualSinkManager.hpp"
#include "audio/pipewire/PwDefaultSinkGuard.hpp"
#include <cstdlib>

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

TEST_CASE("PwVirtualSinkCapture exposes an OS sink and reads silence when idle", "[pipewire]") {
    if (!pwTestAvailable()) { SUCCEED("no PipeWire daemon"); return; }
    crosspad_pc::PwVirtualSinkCapture cap;
    REQUIRE(cap.start("crosspad_test_vin", "CrossPad TEST sink", 48000));
    REQUIRE(cap.isOpen());
    int16_t buf[256 * 2];
    // No app is linked to the test sink — read must not block and returns 0..n.
    uint32_t got = cap.read(buf, 256);
    REQUIRE(got <= 256);
    cap.stop();
    REQUIRE_FALSE(cap.isOpen());
}

TEST_CASE("PwVirtualSinkManager fills IVirtualSinkManager contract", "[pipewire]") {
    if (!pwTestAvailable()) { SUCCEED("no PipeWire daemon"); return; }
    crosspad_pc::PwVirtualSinkManager mgr;
    REQUIRE(mgr.setup(2));
    REQUIRE(mgr.list().size() == 2);
    REQUIRE(mgr.input(0) != nullptr);
    mgr.teardown();          // idempotent
    mgr.teardown();
    REQUIRE(mgr.input(0) == nullptr);
}

TEST_CASE("pwExtractJsonName parses metadata values", "[pipewire][unit]") {
    using crosspad_pc::pwExtractJsonName;
    CHECK(pwExtractJsonName("{\"name\":\"alsa_output.pci.analog-stereo\"}")
          == "alsa_output.pci.analog-stereo");
    CHECK(pwExtractJsonName("{ \"name\" : \"x\" }") == "x");
    CHECK(pwExtractJsonName("garbage") == "");
    CHECK(pwExtractJsonName(nullptr) == "");
}

TEST_CASE("PwDefaultSinkGuard reads current default without mutating", "[pipewire]") {
    if (!pwTestAvailable()) { SUCCEED("no PipeWire daemon"); return; }
    crosspad_pc::PwDefaultSinkGuard guard;
    std::string cur = guard.queryCurrentDefault();
    SUCCEED("current default: " + (cur.empty() ? "<none>" : cur));
}

TEST_CASE("PwDefaultSinkGuard takeover+restore roundtrip", "[pipewire][.mutate]") {
    // Hidden test (leading '.') — only run explicitly; mutates user session.
    if (!pwTestAvailable() || !getenv("CROSSPAD_PW_TEST_MUTATE")) {
        SUCCEED("skipped (set CROSSPAD_PW_TEST_MUTATE=1)"); return;
    }
    crosspad_pc::PwVirtualSinkCapture cap;
    REQUIRE(cap.start("crosspad_test_default", "CrossPad TEST default", 48000));
    crosspad_pc::PwDefaultSinkGuard guard;
    std::string before = guard.queryCurrentDefault();
    REQUIRE(guard.takeover("crosspad_test_default"));
    REQUIRE(guard.previousSink() == before);
    guard.restore();
    REQUIRE(guard.queryCurrentDefault() == before);
    cap.stop();
}
#endif
