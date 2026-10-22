/**
 * @file PcSubprocess.cpp
 * @brief See PcSubprocess.hpp.
 */

#ifndef _WIN32

#include "pc_stubs/PcSubprocess.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace pc_proc {

namespace {

/* NAME of a "NAME=value" entry, or of a bare name. */
std::string env_name(const std::string& entry)
{
    const size_t eq = entry.find('=');
    return eq == std::string::npos ? entry : entry.substr(0, eq);
}

std::vector<std::string> child_env(const Options& opt)
{
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        const std::string name = env_name(*e);
        bool drop = false;
        for (const auto& u : opt.unsetEnv) drop = drop || u == name;
        for (const auto& s : opt.setEnv) drop = drop || env_name(s) == name;
        if (!drop) env.emplace_back(*e);
    }
    for (const auto& s : opt.setEnv) env.push_back(s);
    return env;
}

std::vector<char*> c_strings(std::vector<std::string>& v)
{
    std::vector<char*> out;
    out.reserve(v.size() + 1);
    for (auto& s : v) out.push_back(s.data());
    out.push_back(nullptr);
    return out;
}

/* posix_spawnp() with a clean slate: signals default and unblocked, every
 * descriptor above 2 closed, @p fa's own actions first. */
pid_t spawn_with(std::vector<std::string> argv, std::vector<std::string> env,
                 posix_spawn_file_actions_t* fa, bool newSession)
{
    if (argv.empty()) return -1;
#ifdef __EMSCRIPTEN__
    // A page starts no processes: every caller already takes -1 as "did not start".
    (void)env; (void)fa; (void)newSession;
    return -1;
#else
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(fa, 3);
#endif
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_SETSID
    if (newSession) flags |= POSIX_SPAWN_SETSID;
#else
    (void)newSession;
#endif
    posix_spawnattr_setflags(&attr, flags);

    auto cargv = c_strings(argv);
    auto cenv  = c_strings(env);
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, cargv[0], fa, &attr, cargv.data(), cenv.data());
    posix_spawnattr_destroy(&attr);
    return rc == 0 ? pid : -1;
#endif
}

int remaining_ms(std::chrono::steady_clock::time_point deadline)
{
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

} // namespace

bool reap(pid_t pid, bool block, int& exitCode)
{
    int status = 0;
    for (;;) {
        const pid_t r = waitpid(pid, &status, block ? 0 : WNOHANG);
        if (r == pid) break;
        if (r == 0) return false;               // still running
        if (errno == EINTR) continue;
        exitCode = -1;                          // ECHILD: not ours, or reaped
        return true;
    }
    exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return true;
}

Result run(const std::vector<std::string>& argv, const Options& opt)
{
    Result r;
    int pipefd[2] = {-1, -1};
    if (opt.captureStdout && pipe2(pipefd, O_CLOEXEC) != 0) return r;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (opt.captureStdout) posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    else posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    const pid_t pid = spawn_with(argv, child_env(opt), &fa, false);
    posix_spawn_file_actions_destroy(&fa);
    if (opt.captureStdout) close(pipefd[1]);
    if (pid < 0) {
        if (opt.captureStdout) close(pipefd[0]);
        return r;
    }
    r.started = true;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(opt.timeoutMs);
    if (opt.captureStdout) {
        char buf[4096];
        for (;;) {
            if (opt.timeoutMs > 0) {
                pollfd p{pipefd[0], POLLIN, 0};
                const int pr = poll(&p, 1, remaining_ms(deadline));
                if (pr < 0 && errno == EINTR) continue;
                if (pr == 0) { r.timedOut = true; break; }
            }
            const ssize_t n = read(pipefd[0], buf, sizeof buf);
            if (n > 0) { r.out.append(buf, static_cast<size_t>(n)); continue; }
            if (n < 0 && errno == EINTR) continue;
            break;   // EOF: the child closed its stdout
        }
        close(pipefd[0]);
    }

    if (!r.timedOut && opt.timeoutMs > 0) {
        /* stdout closed (or never captured): the exit, against the deadline. */
        int code = -1;
        while (!reap(pid, false, code)) {
            if (remaining_ms(deadline) == 0) { r.timedOut = true; break; }
            const timespec tick{0, 5 * 1000 * 1000};
            nanosleep(&tick, nullptr);
        }
        if (!r.timedOut) { r.exitCode = code; return r; }
    }
    if (r.timedOut) kill(pid, SIGKILL);
    int code = -1;
    reap(pid, true, code);
    if (!r.timedOut) r.exitCode = code;
    return r;
}

pid_t spawn(const std::vector<std::string>& argv, bool quiet, bool newSession)
{
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (quiet) {
        posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
    const pid_t pid = spawn_with(argv, child_env(Options{}), &fa, newSession);
    posix_spawn_file_actions_destroy(&fa);
    return pid;
}

} // namespace pc_proc

#endif
