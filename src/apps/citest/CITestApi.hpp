// SPDX-License-Identifier: MIT
#pragma once

/**
 * @file CITestApi.hpp
 * @brief Public accessor for the CITest audio pipeline runner.
 *
 * Lets external drivers (RemoteControl TCP, headless CI) start the same
 * test sequence the CITest LVGL app runs, and read back per-stage results
 * without depending on LVGL.
 */

#include <cstdint>
#include <cstddef>

namespace citest {

enum class Result : uint8_t { Pending, Running, Pass, Fail };

/// Spawn the test runner task. Idempotent — returns false if already running.
/// Resets all stage results to Pending. Does NOT require the LVGL UI to be
/// loaded; the runner only touches mixer + synth + audio module.
bool start();

/// True while the runner task is executing.
bool isRunning();

/// Number of stages tracked.
size_t stageCount();

/// Read a stage snapshot. Returns false on out-of-range.
/// `nameOut` and `detailOut` are stable C strings owned by the runner.
bool stageAt(size_t index, const char*& nameOut, Result& resultOut, const char*& detailOut);

} // namespace citest
