#pragma once
/* platform-idf main/include/processMIDI.hpp, for the twin: the radio is the board's. */
#include <cstdint>
inline bool bleMidi_start(uint8_t) { return false; }
inline void bleMidi_stop() {}
inline void bleMidi_applyNoteOffsets() {}
