#pragma once
/*
 * What LVGL does when an assert fires.
 *
 * The default is `while(1);`, and on the simulator that is the worst possible
 * outcome: the LVGL thread spins at full tilt, the window stops redrawing, the
 * log ends mid-line, and the process looks hung while it is in fact running.
 * One of those cost hours — an lv_tlsf.c heap-integrity assert three hours into
 * a session, discovered eight hours later with nothing to attach to, because
 * ptrace_scope blocks gdb on a live process.
 *
 * abort() instead: the [Error] line LVGL already printed stays the last word,
 * a stack goes to stderr, and the core dump is there for gdb afterwards.
 */

#include <stdio.h>
#include <stdlib.h>
#ifdef __linux__
#include <execinfo.h>
#endif

static inline void crosspad_lv_assert_fail(void)
{
    fflush(stdout);
    fputs("\n*** LVGL assert failed — aborting (the [Error] line above says which) ***\n", stderr);
#ifdef __linux__
    /* Needs -rdynamic to come out as names; without it these are addresses for
     * addr2line, which still beats no stack at all. */
    void* frames[32];
    const int n = backtrace(frames, 32);
    backtrace_symbols_fd(frames, n, 2);
#endif
    fflush(stderr);
    abort();
}
