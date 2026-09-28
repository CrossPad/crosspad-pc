#pragma once
/* platform-idf crosspad-platform-idf's CrosspadPlatform.hpp, for the twin:
 * the platform singletons the board's Settings reaches for. The twin has no
 * STM behind them (twin_board.cpp). */
#include "crosspad/protocol/Stm32CommandSender.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#include "crosspad/stm32/Stm32Manager.hpp"

using namespace crosspad;

/* sdkconfig: the board revision string Settings -> Info would name. */
#define CONFIG_BSP_BOARD_REV_STR "twin"

namespace crosspad {
Stm32CommandSender &getStm32CommandSender();
Stm32Manager &getStm32Manager();
} // namespace crosspad
