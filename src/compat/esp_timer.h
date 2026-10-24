#pragma once
// PC shim for esp_timer.h: microseconds since an arbitrary start.
#include <chrono>
#include <cstdint>
// A timer handle as the firmware's headers declare one (fw_slots.h); none is created here.
typedef struct esp_timer *esp_timer_handle_t;
static inline int64_t esp_timer_get_time()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
