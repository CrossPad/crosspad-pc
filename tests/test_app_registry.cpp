#include <catch2/catch_test_macros.hpp>
#include "crosspad/app/AppRegistry.hpp"
#include "crosspad/app/AppEntry.hpp"

using namespace crosspad;

// Helper to create a minimal AppEntry
static AppEntry makeEntry(const char* name, int priority = 0) {
    AppEntry e{};
    e.name = name;
    e.icon = nullptr;
    e.priority = priority;
    return e;
}

TEST_CASE("AppRegistry: register and find", "[app]") {
    auto& registry = AppRegistry::getInstance();

    // Registry may already have entries from static constructors or prior tests.
    // We test relative behavior.
    size_t initialCount = registry.getAppCount();

    SECTION("register an app and find it by name") {
        registry.registerApp(makeEntry("TestApp_Find", 0));

        REQUIRE(registry.getAppCount() == initialCount + 1);

        auto* found = registry.findApp("TestApp_Find");
        REQUIRE(found != nullptr);
        REQUIRE(std::string(found->name) == "TestApp_Find");
    }

    SECTION("findApp returns nullptr for unknown name") {
        auto* found = registry.findApp("NonExistentApp_XYZ");
        REQUIRE(found == nullptr);
    }

    SECTION("getApps returns non-null array") {
        auto* apps = registry.getApps();
        REQUIRE(apps != nullptr);
    }
}

TEST_CASE("AppRegistry: priority selection", "[app]") {
    auto& registry = AppRegistry::getInstance();

    // Register apps with known priorities
    registry.registerApp(makeEntry("LowPrio_Test", 1));
    registry.registerApp(makeEntry("HighPrio_Test", 100));

    auto* selected = registry.getSelectedApp();
    REQUIRE(selected != nullptr);
    REQUIRE(selected->priority >= 100); // highest priority should be selected
}
