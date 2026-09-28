#pragma once
/* crosspad-platform-idf's EspBleMidi.hpp, for the twin: the board's radio as
 * TWIN_STATE reports it; scanning and connecting are the board's. */
#include <cstdint>

#include "crosspad/midi/IBleMidi.hpp"
#include "twin_board.h"

namespace crosspad {
class EspBleMidi {
public:
    static bool isSupported() { return true; }
    const char *getStateName() const { return twin_ble_state(); }
    BleMidiDevice getConnectedDevice() const { BleMidiDevice d; d.address = twin_ble_peer(); return d; }
    void startScan(uint32_t) {}
    void disconnect() {}
};
inline EspBleMidi &getEspBleMidi() { static EspBleMidi b; return b; }
} // namespace crosspad
