/**
 * @file ShutdownSignals.cpp
 * @brief See ShutdownSignals.hpp.
 *
 * crosspad_app_shutdown() talks to PipeWire, runs pactl, takes locks and
 * allocates; the signal handler used to call it. A handler runs on whichever
 * thread the kernel picks, in the middle of whatever that thread was doing --
 * inside malloc, or holding the PipeWire loop lock -- and the shutdown then
 * waited for that lock forever: Ctrl-C or a SIGTERM hung the simulator with
 * the desktop's default sink still on CrossPad's input. The handler now only
 * writes the signal number to a pipe; a thread of its own reads it and runs
 * the shutdown as ordinary code.
 */

#include "pc_stubs/ShutdownSignals.hpp"
#include "crosspad_app.hpp"

#include <csignal>
#include <cstdio>

#ifdef _MSC_VER

/* The CRT runs a SIGINT/SIGTERM handler on a thread of its own, not inside
 * another thread's code, so the shutdown can run there. */
void pc_install_shutdown_signals()
{
    auto handler = [](int sig) {
        crosspad_app_shutdown();
        std::signal(sig, SIG_DFL);
        std::raise(sig);
    };
    std::signal(SIGINT, handler);
    std::signal(SIGTERM, handler);
}

#else

#include <cerrno>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

#include <SDL.h>

namespace {

int s_pipe[2] = {-1, -1};
volatile sig_atomic_t s_signals = 0;

/* Async-signal-safe only: write(), _exit(). */
void on_signal(int sig)
{
    if (++s_signals > 1) _exit(128 + sig);   // a second one: the shutdown hangs, go
    const int saved = errno;
    const unsigned char b = static_cast<unsigned char>(sig);
    (void)!write(s_pipe[1], &b, 1);
    errno = saved;
}

void shutdown_thread()
{
    unsigned char b = 0;
    for (;;) {
        const ssize_t n = read(s_pipe[0], &b, 1);
        if (n == 1) break;
        if (n < 0 && errno == EINTR) continue;
        return;   // the pipe is gone: nothing will ever arrive
    }
    const int sig = b;
    printf("[PC] %s — shutting down\n", sig == SIGINT ? "SIGINT" : "SIGTERM");
    fflush(stdout);
    crosspad_app_shutdown();
    /* Ended by the signal, as the shell and a supervisor expect: default
     * action back, and the signal again to the whole process. */
    struct sigaction dfl = {};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, nullptr);
    kill(getpid(), sig);
    _exit(128 + sig);   // only if every thread has it blocked
}

} // namespace

void pc_install_shutdown_signals()
{
    /* SDL would install its own SIGINT/SIGTERM handlers in SDL_Init(); they
     * turn the signal into an SDL_QUIT that only a running LVGL loop sees. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

    struct sigaction ign = {};
    ign.sa_handler = SIG_IGN;
    sigemptyset(&ign.sa_mask);
    sigaction(SIGPIPE, &ign, nullptr);

    if (pipe2(s_pipe, O_CLOEXEC) != 0) {
        perror("[PC] shutdown signal pipe");
        return;
    }
    std::thread(shutdown_thread).detach();

    struct sigaction sa = {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

#endif
