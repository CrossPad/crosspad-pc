/**
 * @file LinuxPipewireSinks.cpp
 * @brief PulseAudio/PipeWire virtual sink manager.
 *
 * Uses `pactl load-module module-null-sink` to create two null-sinks named
 * "CrossPad virtual IN#1" / "#2". Works on PulseAudio natively and on
 * PipeWire through pipewire-pulse. The sinks' auto-generated .monitor
 * sources are then captured by CrossPad as IN1/IN2.
 *
 * We shell out to `pactl` (via fork+execvp) instead of linking libpulse —
 * zero extra build deps and pactl ships on every distro with PA/PW.
 */

#ifdef __linux__

#include <chrono>
#include <thread>

#include "IVirtualSinkManager.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <map>
#include <mutex>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>

namespace crosspad_pc {

namespace {

struct PactlResult {
    int         exitCode = -1;
    std::string stdoutText;
};

/**
 * Run pactl with the given argv (argv[0] must be "pactl").
 * argv must end with nullptr. Captures stdout, returns exit code.
 */
PactlResult runPactl(const char* const argv[]) {
    PactlResult r;

    int pipefd[2];
    if (pipe(pipefd) != 0) return r;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return r;
    }

    if (pid == 0) {
        // Child — redirect stdout to pipe, discard stderr.
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        close(pipefd[0]);
        close(pipefd[1]);
        // Everything below parses pactl's human-readable output, which is
        // translated. In Polish a sink-input block opens with
        // "4685. odpływ wejścia" — no '#', and the id first — so the header
        // matcher never fired and CrossPad's own output stream could never be
        // found, let alone pinned. Ask for the untranslated output instead of
        // trying to recognise every language.
        setenv("LC_ALL", "C", 1);
        setenv("LANG", "C", 1);
        unsetenv("LANGUAGE");
        execvp("pactl", const_cast<char* const*>(argv));
        _exit(127);
    }

    // Parent — read child stdout, then reap. Retry on EINTR: the FreeRTOS
    // POSIX port delivers SIGALRM/SIGUSR1 for thread scheduling and will
    // interrupt blocking syscalls, which would otherwise leave stdoutText
    // empty even when the child wrote a valid module ID.
    close(pipefd[1]);
    char buf[256];
    for (;;) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n > 0) { r.stdoutText.append(buf, buf + n); continue; }
        if (n == 0) break;
        if (errno == EINTR) continue;
        break;
    }
    close(pipefd[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
    if (WIFEXITED(status)) r.exitCode = WEXITSTATUS(status);
    return r;
}

/// Trim whitespace (incl. trailing newline) in place.
void trim(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    size_t start = 0;
    while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
    if (start > 0) s.erase(0, start);
}

class LinuxPipewireSinks : public IVirtualSinkManager {
public:
    ~LinuxPipewireSinks() override { teardown(); }

    bool setup(uint32_t sinkCount) override {
        std::lock_guard<std::mutex> g(mtx_);
        if (!isAvailable()) {
            printf("[VirtSink] pactl / PulseAudio not available — skipping\n");
            return false;
        }

        // Clean up any stale sinks left over from a previous crashed session.
        cleanupStaleSinks();

        // Capture the existing default sink so we can restore it after loading
        // our null-sinks. Otherwise PipeWire promotes the most recently
        // created null-sink to default — every system stream (Chrome,
        // notifications, even our own OUT2) gets rerouted into it, creating
        // an instant feedback loop through the mixer.
        previousDefaultSink_ = readDefaultSink();

        for (uint32_t i = 0; i < sinkCount; ++i) {
            const std::string sinkName = "crosspad_vin" + std::to_string(i + 1);
            const std::string displayName = "CrossPad virtual IN#" + std::to_string(i + 1);

            // pactl's sink_properties= parser splits the value on the first
            // ASCII space and silently discards the rest — neither double nor
            // single quotes nor backslash escapes work around this in the CLI
            // path. The workaround is to replace interior ASCII spaces with
            // a Unicode non-breaking space (U+00A0, UTF-8 0xC2 0xA0) which
            // GNOME / KDE / GTK audio panels render identically to a normal
            // space, but pactl treats as a non-splitting codepoint.
            std::string descEscaped = displayName;
            const std::string nbsp = "\xC2\xA0";
            for (size_t pos = descEscaped.find(' ');
                 pos != std::string::npos;
                 pos = descEscaped.find(' ', pos + nbsp.size())) {
                descEscaped.replace(pos, 1, nbsp);
            }
            std::string arg_sinkName = "sink_name=" + sinkName;
            std::string arg_sinkProps = "sink_properties=device.description="
                                        + descEscaped;
            const char* const argv[] = {
                "pactl", "load-module", "module-null-sink",
                arg_sinkName.c_str(), arg_sinkProps.c_str(),
                "rate=48000", "channels=2", nullptr
            };
            PactlResult r = runPactl(argv);

            if (r.exitCode != 0) {
                printf("[VirtSink] Failed to load null-sink '%s' (pactl exit %d)\n",
                       sinkName.c_str(), r.exitCode);
                continue;
            }

            trim(r.stdoutText);
            uint32_t moduleId = 0;
            try {
                moduleId = static_cast<uint32_t>(std::stoul(r.stdoutText));
            } catch (...) {
                printf("[VirtSink] Unexpected pactl output: '%s'\n", r.stdoutText.c_str());
                continue;
            }

            VirtualSink s;
            s.displayName       = displayName;
            // Exact PulseAudio source name of the null-sink's monitor — this
            // is what pa_simple_new(PA_STREAM_RECORD, ...) expects. See
            // PulseMonitorCapture for the capture path.
            s.captureDeviceName = sinkName + ".monitor";
            s.channelCount      = 2;
            sinks_.push_back(s);
            loadedModuleIds_.push_back(moduleId);

            printf("[VirtSink] Created '%s' (sink=%s, module=%u)\n",
                   displayName.c_str(), sinkName.c_str(), moduleId);
        }

        // Restore the original default sink so newly-spawned apps (browser,
        // notifications) keep going to the user's real speakers instead of
        // our just-created null-sink.
        if (!previousDefaultSink_.empty()) {
            const char* const argv[] = {
                "pactl", "set-default-sink", previousDefaultSink_.c_str(), nullptr
            };
            PactlResult r = runPactl(argv);
            if (r.exitCode == 0) {
                printf("[VirtSink] Restored default sink: %s\n",
                       previousDefaultSink_.c_str());
            }
        }

        return !sinks_.empty();
    }

    void teardown() override {
        std::lock_guard<std::mutex> g(mtx_);
        for (uint32_t id : loadedModuleIds_) {
            std::string idStr = std::to_string(id);
            const char* const argv[] = { "pactl", "unload-module", idStr.c_str(), nullptr };
            PactlResult r = runPactl(argv);
            if (r.exitCode != 0) {
                printf("[VirtSink] unload-module %u failed (exit %d)\n", id, r.exitCode);
            } else {
                printf("[VirtSink] Unloaded module %u\n", id);
            }
        }
        loadedModuleIds_.clear();
        sinks_.clear();
    }

    std::vector<VirtualSink> list() const override {
        std::lock_guard<std::mutex> g(mtx_);
        return sinks_;
    }

    bool isAvailable() const override {
        const char* const argv[] = { "pactl", "info", nullptr };
        PactlResult r = runPactl(argv);
        return r.exitCode == 0;
    }

    std::string errorHint() const override {
        return "pactl not found or PulseAudio/PipeWire daemon not running. "
               "Install 'pulseaudio-utils' (or 'pipewire-pulse') and ensure "
               "the user session has an active audio daemon.";
    }

private:
    /**
     * Remove any crosspad_vin* modules left over from a prior crashed run.
     * Parses `pactl list modules short` looking for module-null-sink rows
     * whose arguments reference a crosspad_vin sink_name.
     */
    void cleanupStaleSinks() {
        const char* const argv[] = { "pactl", "list", "modules", "short", nullptr };
        PactlResult r = runPactl(argv);
        if (r.exitCode != 0) return;

        std::istringstream ss(r.stdoutText);
        std::string line;
        while (std::getline(ss, line)) {
            // Format: <id>\tmodule-null-sink\t<args>
            if (line.find("module-null-sink") == std::string::npos) continue;
            if (line.find("crosspad_vin") == std::string::npos) continue;
            size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string idStr = line.substr(0, tab);
            const char* const unloadArgv[] = {
                "pactl", "unload-module", idStr.c_str(), nullptr
            };
            runPactl(unloadArgv);
            printf("[VirtSink] Cleaned up stale module %s\n", idStr.c_str());
        }
    }

    /// Read the current PulseAudio/PipeWire default sink name.
    std::string readDefaultSink() const {
        const char* const argv[] = { "pactl", "get-default-sink", nullptr };
        PactlResult r = runPactl(argv);
        if (r.exitCode != 0) return {};
        std::string name = r.stdoutText;
        trim(name);
        return name;
    }

    mutable std::mutex       mtx_;
    std::vector<VirtualSink> sinks_;
    std::vector<uint32_t>    loadedModuleIds_;
    std::string              previousDefaultSink_;
};

} // namespace

std::unique_ptr<IVirtualSinkManager> makeLinuxPipewireSinks() {
    return std::make_unique<LinuxPipewireSinks>();
}

std::vector<PulseSinkInfo> enumeratePulseSinks(bool includeUnavailable) {
    std::vector<PulseSinkInfo> result;

    // Names from `pactl list sinks short`.
    const char* const argvShort[] = { "pactl", "list", "sinks", "short", nullptr };
    PactlResult shortR = runPactl(argvShort);
    if (shortR.exitCode != 0) return result;

    std::vector<std::string> names;
    {
        std::istringstream ss(shortR.stdoutText);
        std::string line;
        while (std::getline(ss, line)) {
            size_t t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            size_t t2 = line.find('\t', t1 + 1);
            if (t2 == std::string::npos) continue;
            std::string name = line.substr(t1 + 1, t2 - t1 - 1);
            // Filter our own null-sinks — listing them as outputs would let
            // the user wire OUT1 → vin1.monitor → IN1, an instant feedback loop.
            if (name.rfind("crosspad_vin", 0) == 0) continue;
            names.push_back(name);
        }
    }

    // Parse verbose `pactl list sinks` once for descriptions + active-port
    // availability. pactl is locale-aware — accept English and Polish.
    const char* const argvFull[] = { "pactl", "list", "sinks", nullptr };
    PactlResult fullR = runPactl(argvFull);

    struct SinkMeta { std::string desc; bool available = true; };
    std::map<std::string, SinkMeta> meta;

    if (fullR.exitCode == 0) {
        std::istringstream ss(fullR.stdoutText);
        std::string l, curName, curDesc, curActivePort;
        // Per-sink port-availability map: portName -> available?
        std::map<std::string, bool> portAvail;

        auto flushSink = [&]() {
            if (curName.empty()) return;
            SinkMeta m;
            m.desc = curDesc;
            // If we recorded an active port and saw its availability line, use it.
            // Default: available=true (works for sinks without port concept,
            // e.g. null-sinks, software loopbacks).
            if (!curActivePort.empty()) {
                auto it = portAvail.find(curActivePort);
                if (it != portAvail.end()) m.available = it->second;
            }
            meta[curName] = m;
            curName.clear(); curDesc.clear(); curActivePort.clear();
            portAvail.clear();
        };

        while (std::getline(ss, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            // New sink section starts with "Sink #<id>" / "Odpływ #<id>".
            // Flush whatever we accumulated for the previous one.
            if (l.rfind("Sink #", 0) == 0 || l.rfind("Odpływ #", 0) == 0) {
                flushSink();
                continue;
            }
            size_t p;
            if ((p = l.find("Name: ")) != std::string::npos) {
                curName = l.substr(p + 6); trim(curName);
            } else if ((p = l.find("Nazwa: ")) != std::string::npos) {
                curName = l.substr(p + 7); trim(curName);
            } else if ((p = l.find("Description: ")) != std::string::npos) {
                curDesc = l.substr(p + 13); trim(curDesc);
            } else if ((p = l.find("Opis: ")) != std::string::npos) {
                curDesc = l.substr(p + 6); trim(curDesc);
            } else if ((p = l.find("Active Port: ")) != std::string::npos) {
                curActivePort = l.substr(p + 13); trim(curActivePort);
            } else if ((p = l.find("Aktywny port: ")) != std::string::npos) {
                curActivePort = l.substr(p + 14); trim(curActivePort);
            } else {
                // Port lines look like:
                //   "\thdmi-output-0: HDMI / DisplayPort (..., not available)"
                //   "\tanalog-output-lineout: Line Out (..., available)"
                // Detect port name as "<word>:" at start (after whitespace)
                // and pull availability from substring of same line.
                size_t colon = l.find(':');
                size_t firstNonWs = l.find_first_not_of(" \t");
                if (colon != std::string::npos && firstNonWs != std::string::npos &&
                    colon > firstNonWs) {
                    std::string token = l.substr(firstNonWs, colon - firstNonWs);
                    // Heuristic: real port names contain a hyphen and no spaces.
                    if (!token.empty() && token.find(' ') == std::string::npos &&
                        token.find('-') != std::string::npos) {
                        bool avail = true;
                        // English "not available" / Polish "niedostępne".
                        if (l.find("not available") != std::string::npos ||
                            l.find("niedostępn") != std::string::npos) {
                            avail = false;
                        }
                        portAvail[token] = avail;
                    }
                }
            }
        }
        flushSink();
    }

    for (const auto& n : names) {
        auto it = meta.find(n);
        PulseSinkInfo info;
        info.name        = n;
        info.description = (it != meta.end() && !it->second.desc.empty())
                           ? it->second.desc : n;
        info.available   = (it != meta.end()) ? it->second.available : true;
        if (!includeUnavailable && !info.available) continue;
        result.push_back(std::move(info));
    }
    return result;
}

static bool tryMovePulseOutputToSink(int slot, const std::string& targetSinkName);

bool movePulseOutputToSink(int slot, const std::string& targetSinkName) {
    if (slot < 0 || slot > 1 || targetSinkName.empty()) return false;
    return movePulseOutputToSinkWithin(slot, targetSinkName, 1500);
}

bool movePulseOutputToSinkWithin(int slot, const std::string& targetSinkName,
                                 int timeoutMs) {
    if (slot < 0 || slot > 1 || targetSinkName.empty()) return false;

    // The stream is created asynchronously: RtAudio's open returns before the
    // sound server has a node for it, so a single look can find nothing and
    // report a failure that never happened. Poll until it shows up.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
    for (;;) {
        if (tryMovePulseOutputToSink(slot, targetSinkName)) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

static bool tryMovePulseOutputToSink(int slot, const std::string& targetSinkName) {
    // Find CrossPad sink-inputs in creation order. PulseAudio assigns IDs
    // monotonically per session, so the first ID belongs to OUT1, the second
    // to OUT2.
    const char* const argvList[] = { "pactl", "list", "sink-inputs", nullptr };
    PactlResult r = runPactl(argvList);
    if (r.exitCode != 0) return false;

    std::vector<std::string> crosspadInputIds;
    {
        std::istringstream ss(r.stdoutText);
        std::string l, currentId;
        bool isCrossPad = false;
        auto flush = [&]() {
            if (isCrossPad && !currentId.empty()) {
                crosspadInputIds.push_back(currentId);
            }
            currentId.clear();
            isCrossPad = false;
        };
        while (std::getline(ss, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            // Section header: "Sink Input #<id>" / "Wejście odpływu #<id>"
            size_t hash = l.find('#');
            if (hash != std::string::npos &&
                (l.find("Sink Input") != std::string::npos ||
                 l.find("ejście") != std::string::npos /* "Wejście"|"wejście" */)) {
                flush();
                currentId = l.substr(hash + 1);
                trim(currentId);
                continue;
            }
            // Application name lines vary by locale too — match by literal
            // application.name property which is locale-stable.
            if (l.find("application.name = \"CrossPad\"") != std::string::npos ||
                l.find("application.name = \"PipeWire ALSA [CrossPad]\"") != std::string::npos ||
                l.find("application.name = \"ALSA plug-in [CrossPad]\"") != std::string::npos) {
                isCrossPad = true;
            }
        }
        flush();
    }

    if (static_cast<size_t>(slot) >= crosspadInputIds.size()) {
        return false;   // not there yet; the caller polls
    }

    const std::string& sid = crosspadInputIds[slot];
    const char* const argvMove[] = {
        "pactl", "move-sink-input", sid.c_str(), targetSinkName.c_str(), nullptr
    };
    PactlResult mr = runPactl(argvMove);
    if (mr.exitCode != 0) {
        printf("[VirtSink] move-sink-input %s -> %s failed (exit %d)\n",
               sid.c_str(), targetSinkName.c_str(), mr.exitCode);
        return false;
    }
    printf("[VirtSink] Moved CrossPad OUT%d (sink-input %s) -> %s\n",
           slot + 1, sid.c_str(), targetSinkName.c_str());
    return true;
}

} // namespace crosspad_pc

#endif // __linux__
