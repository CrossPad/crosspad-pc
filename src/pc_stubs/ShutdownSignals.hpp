/**
 * @file ShutdownSignals.hpp
 * @brief SIGINT / SIGTERM end the simulator through crosspad_app_shutdown().
 */
#pragma once

/// Before any thread exists (first thing in main()). SIGINT and SIGTERM then
/// run crosspad_app_shutdown() -- the desktop's default sink back, CrossPad's
/// sinks and nodes gone -- and end the process by that signal; a second one
/// while that runs ends it at once. SIGPIPE is ignored: a control-port client
/// that hangs up must not take the simulator with it.
void pc_install_shutdown_signals();
