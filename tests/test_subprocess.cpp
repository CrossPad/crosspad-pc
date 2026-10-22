/**
 * @file test_subprocess.cpp
 * @brief pc_proc: argv/env built in the parent, EINTR-proof waits, timeouts,
 *        no inherited descriptors. Runs inside the FreeRTOS test task, so the
 *        port's SIGALRM ticks interrupt every blocking call it makes.
 */

#ifndef _WIN32

#include <catch2/catch_test_macros.hpp>

#include "pc_stubs/PcSubprocess.hpp"

#include <chrono>
#include <cstdlib>
#include <string>

#include <fcntl.h>
#include <unistd.h>

TEST_CASE("pc_proc::run captures stdout and the exit status", "[subprocess]") {
    const auto r = pc_proc::run({"sh", "-c", "printf hello; exit 3"});
    REQUIRE(r.started);
    CHECK_FALSE(r.timedOut);
    CHECK(r.exitCode == 3);
    CHECK(r.out == "hello");
}

TEST_CASE("pc_proc::run reads a long output through the scheduler's ticks", "[subprocess]") {
    // A pause first, so the 1 kHz SIGALRM lands while the read is blocked.
    const auto r = pc_proc::run({"sh", "-c", "sleep 0.2; head -c 200000 /dev/zero | tr '\\0' x"});
    REQUIRE(r.started);
    CHECK(r.exitCode == 0);
    CHECK(r.out.size() == 200000);
}

TEST_CASE("pc_proc::run kills a child that outlives its timeout", "[subprocess]") {
    pc_proc::Options opt;
    opt.timeoutMs = 200;
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = pc_proc::run({"sleep", "5"}, opt);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    REQUIRE(r.started);
    CHECK(r.timedOut);
    CHECK(r.exitCode == -1);
    CHECK(ms < 2000);

    opt.captureStdout = false;
    const auto quiet = pc_proc::run({"sleep", "5"}, opt);
    CHECK(quiet.timedOut);
}

TEST_CASE("pc_proc::run reports a program that is not there", "[subprocess]") {
    const auto r = pc_proc::run({"/nonexistent/crosspad-no-such-tool"});
    CHECK_FALSE(r.started);
    CHECK(r.exitCode == -1);
}

TEST_CASE("pc_proc::run builds the child's environment in the parent", "[subprocess]") {
    pc_proc::Options opt;
    opt.unsetEnv = {"HOME"};
    opt.setEnv   = {"CP_PROC_TEST=1", "LANG=C"};
    const auto r = pc_proc::run({"sh", "-c", "printf '%s-%s-%s' \"$CP_PROC_TEST\" \"${HOME:-none}\" \"$LANG\""}, opt);
    REQUIRE(r.started);
    CHECK(r.out == "1-none-C");
}

#ifdef __linux__
TEST_CASE("pc_proc children inherit no descriptor but 0, 1 and 2", "[subprocess]") {
    // A descriptor without O_CLOEXEC, as a socket or a pipe some library
    // opened would be.
    const int fd = open("/dev/null", O_RDONLY);
    REQUIRE(fd > 2);
    const std::string probe = "[ -e /proc/self/fd/" + std::to_string(fd) + " ] && echo open || echo closed";
    const auto r = pc_proc::run({"sh", "-c", probe});
    close(fd);
    REQUIRE(r.started);
    CHECK(r.out == "closed\n");
}
#endif

TEST_CASE("pc_proc::spawn and reap", "[subprocess]") {
    const pid_t pid = pc_proc::spawn({"sh", "-c", "sleep 0.1; exit 7"}, /*quiet=*/true, /*newSession=*/true);
    REQUIRE(pid > 0);
    int code = -1;
    CHECK_FALSE(pc_proc::reap(pid, /*block=*/false, code));
    CHECK(pc_proc::reap(pid, /*block=*/true, code));
    CHECK(code == 7);
    // Reaped already: no child of ours any more.
    CHECK(pc_proc::reap(pid, /*block=*/true, code));
    CHECK(code == -1);
}

#endif
