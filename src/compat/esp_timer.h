#pragma once
// PC shim for esp_timer.h: microseconds since an arbitrary start.
#include <chrono>
#include <cstdint>
static inline int64_t esp_timer_get_time()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
