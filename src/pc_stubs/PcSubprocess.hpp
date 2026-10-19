/**
 * @file PcSubprocess.hpp
 * @brief Child processes for a many-threaded process (POSIX).
 *
 * The simulator runs thirty-odd threads; the child of a fork() may only make
 * async-signal-safe calls until it execs, and one that called setenv() there
 * waited for an allocator lock another thread held at the moment of the fork
 * -- for ever (47f6c4b). Everything here goes through posix_spawnp() with an
 * argv and an environment built in the parent, so nothing of ours runs in the
 * child. The child gets default signal dispositions and an empty mask (the
 * FreeRTOS POSIX port blocks and handles signals of its own), and no file
 * descriptor but 0, 1 and 2: the control port's socket, the PipeWire and X11
 * connections stay ours. Every wait retries on EINTR -- the port's SIGALRM
 * interrupts blocking calls on its task threads without SA_RESTART.
 */
#pragma once

#ifndef _WIN32

#include <string>
#include <vector>
#include <sys/types.h>

namespace pc_proc {

struct Options {
    /// Removed from the child's environment.
    std::vector<std::string> unsetEnv;
    /// "NAME=value", added to the child's environment (replacing NAME).
    std::vector<std::string> setEnv;
    /// The child is killed (SIGKILL) after this long; 0 waits for ever.
    int  timeoutMs = 0;
    /// stdout into Result::out; otherwise /dev/null.
    bool captureStdout = true;
};

struct Result {
    bool        started  = false;   ///< posix_spawnp() succeeded
    bool        timedOut = false;   ///< killed at Options::timeoutMs
    int         exitCode = -1;      ///< the exit status; -1 unless it exited
    std::string out;                ///< stdout, when captured
};

/// Run @p argv (argv[0] looked up in PATH) and wait for it; stderr to
/// /dev/null.
Result run(const std::vector<std::string>& argv, const Options& opt = {});

/// Start @p argv and return at once: its pid, or -1. stdin, stdout and
/// stderr are ours, or /dev/null with @p quiet; @p newSession puts it in a
/// session of its own (setsid), so it outlives us and our terminal.
pid_t spawn(const std::vector<std::string>& argv, bool quiet, bool newSession);

/// Reap @p pid: true once it has ended, with @p exitCode its status (-1
/// when it did not exit normally, or is no child of ours). @p block waits for
/// that; otherwise false while it still runs.
bool reap(pid_t pid, bool block, int& exitCode);

} // namespace pc_proc

#endif
