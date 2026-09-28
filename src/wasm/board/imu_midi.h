#pragma once
/* platform-idf main/include/imu_midi.h, for the twin: the IMU is the board's. */
#include <cstdint>
#include "twin_board.h"
inline void imu_midi_set_enabled(bool) {}
inline uint32_t imu_midi_sent(void) { return twin_imu_sent(); }
