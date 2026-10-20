// SPDX-License-Identifier: GPL-3.0-or-later
#include "fw/PcFirmware.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <ArduinoJson.h>

#include "crosspad_app.hpp"
#include "crosspad-gui/components/boot_splash.h"
#include "crosspad-gui/components/ui_toast.h"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "pc_stubs/PcSubprocess.hpp"
#include "pc_stubs/pc_platform.h"
#include "updater/PcUpdater.hpp"

#include "lvgl.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;
using crosspad::FwPkgVerdict;
using crosspad::FwSlotInfo;
using crosspad::FwSlotState;

namespace pc_fw {

namespace {

constexpr const char* kProject = "crosspad-pc";
constexpr const char* kSlotArg = "--fw-slot=";
constexpr const char* kRecoveryArg = "--recovery";
constexpr const char* kBootArg = "--boot";
constexpr const char* kComplete = ".complete";
constexpr const char* kDownloading = ".downloading";
constexpr const char* kPinned = ".pinned";
constexpr const char* kBoardImage = "CrossPad.bin";
constexpr const char* kManifest = "manifest.json";
constexpr const char* kIcon = "icon.bin";
constexpr uintmax_t kIconBytesMax = 16 * 1024;   // what the board takes for icon.bin
/// A trial boot confirms itself this long after the boot finished, as the
/// board's image does once it has come up.
constexpr uint32_t kConfirmDelayMs = 5000;
constexpr uint32_t kRollbackToastMs = 5000;
constexpr uint32_t kToastPollMs = 200;
constexpr size_t kScanChunk = 1u << 20;
constexpr size_t kDescMax = 256;

struct SlotRec {
    std::string exe;       ///< the binary this slot boots
    std::string package;   ///< "" = flashed over USB, unnamed
    std::string kit, version;
    FwSlotState state = FwSlotState::Empty;
    int attempts = 0;      ///< trial starts that never confirmed
};

SlotRec s_slots[2];
int s_running = 0;
int s_boot = 0;
int s_argSlot = -1;
bool s_argRecovery = false;
bool s_argBoot = false;
char s_rolledBack = 0;   ///< the slot the bootloader rolled back, not told yet
FwSlotInfo s_info[2]{};

int other(int slot) { return slot ^ 1; }
char label(int slot) { return slot == 0 ? 'A' : 'B'; }

std::string profileDir() { return pc_platform_resolve_profile_dir(); }
std::string stateDir() { return profileDir() + "/fw"; }
std::string preferencesFile() { return profileDir() + "/preferences.json"; }
std::string slotsFile() { return stateDir() + "/slots.json"; }
std::string slotImage(int slot) { return stateDir() + "/slot_" + static_cast<char>('a' + slot) + "/" + kImageName; }

const char* stateName(FwSlotState s) {
    switch (s) {
        case FwSlotState::Empty: return "empty";
        case FwSlotState::Installed: return "installed";
        case FwSlotState::Trial: return "trial";
        case FwSlotState::Confirmed: return "confirmed";
        case FwSlotState::RolledBack: return "rolled_back";
    }
    return "empty";
}

FwSlotState stateFrom(const char* name) {
    if (!name) return FwSlotState::Empty;
    for (FwSlotState s : {FwSlotState::Installed, FwSlotState::Trial, FwSlotState::Confirmed, FwSlotState::RolledBack})
        if (std::strcmp(name, stateName(s)) == 0) return s;
    return FwSlotState::Empty;
}

void load() {
    std::ifstream f(slotsFile());
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f)) return;
    s_boot = std::strcmp(doc["boot"] | "A", "B") == 0 ? 1 : 0;
    const char* rolled = doc["rolled_back"] | "";
    s_rolledBack = rolled[0] == 'A' || rolled[0] == 'B' ? rolled[0] : 0;
    for (int i = 0; i < 2; ++i) {
        JsonObjectConst o = doc["slots"][std::string(1, label(i))];
        SlotRec& r = s_slots[i];
        r.exe = o["exe"] | "";
        r.package = o["package"] | "";
        r.kit = o["kit"] | "";
        r.version = o["version"] | "";
        r.state = stateFrom(o["state"] | "empty");
        r.attempts = o["attempts"] | 0;
    }
}

void save() {
    std::error_code ec;
    fs::create_directories(stateDir(), ec);
    JsonDocument doc;
    doc["boot"] = std::string(1, label(s_boot));
    doc["rolled_back"] = s_rolledBack ? std::string(1, s_rolledBack) : std::string();
    for (int i = 0; i < 2; ++i) {
        const SlotRec& r = s_slots[i];
        JsonObject o = doc["slots"][std::string(1, label(i))].to<JsonObject>();
        o["exe"] = r.exe;
        o["package"] = r.package;
        o["kit"] = r.kit;
        o["version"] = r.version;
        o["state"] = stateName(r.state);
        o["attempts"] = r.attempts;
    }
    const std::string tmp = slotsFile() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        serializeJsonPretty(doc, f);
    }
    fs::rename(tmp, slotsFile(), ec);
    if (ec) std::printf("[Firmware] could not save %s: %s\n", slotsFile().c_str(), ec.message().c_str());
}

void refreshInfo() {
    for (int i = 0; i < 2; ++i) {
        const SlotRec& r = s_slots[i];
        FwSlotInfo& s = s_info[i];
        s = FwSlotInfo{};
        s.label = label(i);
        s.running = i == s_running;
        s.state = r.state;
        std::snprintf(s.package, sizeof s.package, "%s", r.package.c_str());
        std::snprintf(s.kit, sizeof s.kit, "%s", r.kit.c_str());
        std::snprintf(s.version, sizeof s.version, "%s", r.version.c_str());
        s.hasDesc = r.state != FwSlotState::Empty;
    }
}

/// Where @p slot's firmware keeps its settings: by package, as the board keeps
/// them in the package; an image flashed over USB has its kit for a name.
std::string settingsFile(int slot) {
    const SlotRec& r = s_slots[slot];
    return stateDir() + "/settings/" + (r.package.empty() ? "usb-" + r.kit : r.package) + ".json";
}

void copySettings(const std::string& from, const std::string& to) {
    std::error_code ec;
    if (!fs::exists(from, ec)) return;
    fs::create_directories(fs::path(to).parent_path(), ec);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) std::printf("[Firmware] settings not copied to %s: %s\n", to.c_str(), ec.message().c_str());
}

bool slotBootable(int slot) {
    std::error_code ec;
    return s_slots[slot].state != FwSlotState::Empty && fs::exists(s_slots[slot].exe, ec);
}

/* The board's assets partition is shared by both slots. A slot's binary has
 * none beside it (fw/slot_x), so it is told where this one's are; one that
 * was told itself passes it on through the environment. */
void shareAssets() {
    if (std::getenv("CROSSPAD_ASSETS")) return;
    const fs::path dir = fs::path(PcUpdater::getCurrentExePath()).parent_path() / "assets";
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
#ifdef _WIN32
    _putenv_s("CROSSPAD_ASSETS", dir.string().c_str());
#else
    setenv("CROSSPAD_ASSETS", dir.string().c_str(), 1);
#endif
}

#ifdef _WIN32
std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
#endif

/* Starts @p argv and returns at once; it inherits our environment. */
bool launch(const std::vector<std::string>& argv) {
#ifdef _WIN32
    std::wstring cmd;
    for (const std::string& a : argv) cmd += L"\"" + wide(a) + L"\" ";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    // A slot's copy has no SDL2.dll beside it: started in this binary's
    // directory, the DLL search finds this one's.
    const std::wstring cwd = wide(fs::path(PcUpdater::getCurrentExePath()).parent_path().string());
    if (!CreateProcessW(wide(argv[0]).c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, cwd.c_str(), &si,
                        &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    return pc_proc::spawn(argv, /*quiet=*/false, /*newSession=*/false) >= 0;
#endif
}

/* @p slot's binary, told its slot; the environment (profile, control port,
 * assets) is ours. */
bool spawnSlot(int slot) {
    const SlotRec& r = s_slots[slot];
    const std::string slotArg = std::string(kSlotArg) + label(slot);
    shareAssets();
    if (!launch({r.exe, slotArg})) {
        std::printf("[Firmware] could not start %s\n", r.exe.c_str());
        return false;
    }
    std::printf("[Firmware] starting slot %c: %s %s (%s)\n", label(slot), r.kit.c_str(), r.version.c_str(),
                r.exe.c_str());
    std::fflush(stdout);
    return true;
}

/* The restart into @p slot: its binary is started, this process ends. */
bool startSlot(int slot) {
    if (!spawnSlot(slot)) return false;
    crosspad_app_shutdown();
    std::_Exit(0);
}

/* The board's switch: the target becomes the boot slot, on trial unless it
 * confirmed itself before. */
bool switchTo(int slot, bool clearRollback, bool force) {
    if (!force && s_slots[s_running].state == FwSlotState::Trial) {
        std::printf("[Firmware] switch refused: this firmware has not confirmed itself yet\n");
        return false;
    }
    if (!slotBootable(slot)) {
        std::printf("[Firmware] switch refused: slot %c holds no firmware\n", label(slot));
        return false;
    }
    SlotRec& r = s_slots[slot];
    if (r.state == FwSlotState::RolledBack && !clearRollback && !force) {
        std::printf("[Firmware] switch refused: slot %c rolled back\n", label(slot));
        return false;
    }
    const SlotRec before = r;
    const int bootBefore = s_boot;
    if (r.state != FwSlotState::Confirmed) {
        r.state = FwSlotState::Trial;
        r.attempts = 1;   // the bootloader's count: this restart is the first start
    }
    s_boot = slot;
    save();
    copySettings(preferencesFile(), settingsFile(s_running));
    if (startSlot(slot)) return true;
    r = before;
    s_boot = bootBefore;
    save();
    refreshInfo();
    return false;
}

std::string findDesc(std::ifstream& f) {
    /* The marker is put together here, so this binary holds it only once:
     * in its own descriptor. */
    const std::string marker = std::string("CPFI-PC") + "1;";
    std::string buf;
    std::vector<char> chunk(kScanChunk);
    while (f) {
        f.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        buf.append(chunk.data(), static_cast<size_t>(f.gcount()));
        const size_t at = buf.find(marker);
        if (at != std::string::npos) {
            if (buf.size() - at < kDescMax && f) continue;   // the rest of it in the next chunk
            const size_t end = buf.find('\n', at);
            return buf.substr(at + marker.size(), end == std::string::npos ? kDescMax : end - at - marker.size());
        }
        if (buf.size() > marker.size()) buf.erase(0, buf.size() - marker.size());
    }
    return {};
}

std::string field(const std::string& desc, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t at = desc.find(k);
    while (at != std::string::npos && at != 0 && desc[at - 1] != ';') at = desc.find(k, at + 1);
    if (at == std::string::npos) return {};
    const size_t end = desc.find(';', at);
    return desc.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size());
}

uint64_t dirBytes(const fs::path& dir) {
    uint64_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) n += e.file_size(ec);
    return n;
}

/* The bootloader's rollback, told once the splash is gone (under it the
 * toast would come and go unseen). */
void rollbackToastTimer(lv_timer_t* t) {
    if (boot_splash_active()) return;
    lv_timer_delete(t);
    static char text[64];
    std::snprintf(text, sizeof text, "Slot %c did not start - back on slot %c", s_rolledBack, label(s_running));
    ui_toast_show(text, kRollbackToastMs, UI_TOAST_WARNING);
    s_rolledBack = 0;
    save();
}

void confirmTimer(lv_timer_t* t) {
    lv_timer_delete(t);
    SlotRec& r = s_slots[s_running];
    if (r.state != FwSlotState::Trial) return;
    r.state = FwSlotState::Confirmed;
    r.attempts = 0;
    save();
    refreshInfo();
    std::printf("[Firmware] slot %c confirmed\n", label(s_running));
}

} // namespace

ImageDesc readImageDesc(const std::string& path) {
    ImageDesc d;
    std::ifstream f(path, std::ios::binary);
    if (!f) return d;
    const std::string desc = findDesc(f);
    if (desc.empty()) return d;
    d.found = true;
    d.kit = field(desc, "kit");
    d.version = field(desc, "version");
    d.name = field(desc, "name");
    d.project = field(desc, "project");
    return d;
}

ImageDesc ownDesc() {
    ImageDesc d;
    d.found = true;
    d.kit = CROSSPAD_FW_KIT_ID;
    d.version = CROSSPAD_FW_KIT_VERSION;
    d.name = CROSSPAD_FW_KIT_NAME;
    d.project = kProject;
    return d;
}

void parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strncmp(argv[i], kSlotArg, std::strlen(kSlotArg)) == 0) {
            const char c = argv[i][std::strlen(kSlotArg)];
            if (c == 'A' || c == 'B') s_argSlot = c - 'A';
        } else if (std::strcmp(argv[i], kRecoveryArg) == 0) {
            s_argRecovery = true;
        } else if (std::strcmp(argv[i], kBootArg) == 0) {
            s_argBoot = true;
        }
    }
}

bool runBootloader() {
    if (!s_argBoot) return false;
    load();
    int slot = s_boot;
    if (!slotBootable(slot)) {
        std::printf("[Firmware] boot: slot %c holds no firmware, starting this binary\n", label(slot));
        return false;
    }
    SlotRec& r = s_slots[slot];
    if (r.state == FwSlotState::Trial && r.attempts >= 1) {
        // Started on trial before and never confirmed: the board's bootloader
        // marks the slot invalid and boots the other one.
        r.state = FwSlotState::RolledBack;
        r.attempts = 0;
        if (slotBootable(other(slot))) {
            std::printf("[Firmware] boot: slot %c did not confirm itself, rolling back to slot %c\n", label(slot),
                        label(other(slot)));
            s_rolledBack = label(slot);
            slot = other(slot);
            s_boot = slot;
        }
    } else if (r.state == FwSlotState::Trial) {
        ++r.attempts;
    }
    save();
    return spawnSlot(slot);
}

void boot() {
    load();
    const ImageDesc own = ownDesc();
    if (s_argSlot >= 0) {
        s_running = s_argSlot;
    } else {
        // Started by hand: flashed over USB into slot A, which boots.
        s_running = 0;
        s_slots[0].exe = PcUpdater::getCurrentExePath();
        s_slots[0].package.clear();
        s_slots[0].state = FwSlotState::Confirmed;
    }
    // The image is the authority over what the record says, as CPFI is.
    SlotRec& r = s_slots[s_running];
    r.kit = own.kit;
    r.version = own.version;
    if (r.exe.empty()) r.exe = PcUpdater::getCurrentExePath();
    if (r.state == FwSlotState::Empty) r.state = FwSlotState::Confirmed;
    s_boot = s_running;
    save();
    refreshInfo();
    // A slot a switch started takes its own settings back (the board's
    // fw_switch_finish_boot()); one flashed by hand keeps what is there.
    if (s_argSlot >= 0) copySettings(settingsFile(s_running), preferencesFile());
    std::printf("[Firmware] slot %c: %s %s%s\n", label(s_running), r.kit.c_str(), r.version.c_str(),
                r.state == FwSlotState::Trial ? " (trial)" : "");
}

bool bootFinished(bool pad16Held) {
    if (pad16Held || s_argRecovery) {
        const int main = other(s_running);
        if (s_slots[s_running].kit == kMainKit) {
            std::printf("[Firmware] recovery: the main kit is running\n");
        } else if (s_slots[main].kit != kMainKit || !slotBootable(main)) {
            std::printf("[Firmware] recovery: no slot holds the main kit (%s)\n", kMainKit);
        } else {
            std::printf("[Firmware] recovery: pad 16 held at boot, back to slot %c (%s)\n", label(main), kMainKit);
            return switchTo(main, true, true);
        }
    }
    if (s_slots[s_running].state == FwSlotState::Trial) lv_timer_create(confirmTimer, kConfirmDelayMs, nullptr);
    if (s_rolledBack) lv_timer_create(rollbackToastTimer, kToastPollMs, nullptr);
    return false;
}

void readSlots(FwSlotInfo out[2]) {
    out[0] = s_info[0];
    out[1] = s_info[1];
}

const FwSlotInfo& runningSlot() { return s_info[s_running]; }

bool runningOnTrial() { return s_slots[s_running].state == FwSlotState::Trial; }

std::string libraryRoot() {
    const std::string card = pc_platform_get_sdcard_path();
    return card.empty() ? std::string() : card + "/crosspad/firmware";
}

std::vector<Package> scanLibrary() {
    std::vector<Package> out;
    const std::string root = libraryRoot();
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) return out;
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (!e.is_directory(ec)) continue;
        Package p;
        p.name = e.path().filename().string();
        const fs::path dir = e.path();
        p.pinned = fs::exists(dir / kPinned, ec);
        p.hasIcon = fs::exists(dir / kIcon, ec);
        p.complete = !fs::exists(dir / kDownloading, ec) || fs::exists(dir / kComplete, ec);
        p.bytes = dirBytes(dir);
        if (!p.complete) {
            p.verdict = FwPkgVerdict::Downloading;
        } else if (fs::exists(dir / kImageName, ec)) {
            p.desc = readImageDesc((dir / kImageName).string());
            if (!p.desc.found) p.verdict = FwPkgVerdict::NotAnImage;
            else if (p.desc.project != kProject) p.verdict = FwPkgVerdict::WrongProject;
            else p.verdict = FwPkgVerdict::Ok;
        } else {
            // A board image is another project's here.
            p.verdict = fs::exists(dir / kBoardImage, ec) ? FwPkgVerdict::WrongProject : FwPkgVerdict::NoImage;
        }
        out.push_back(std::move(p));
    }
    return out;
}

PackageText readPackageText(const std::string& pkg) {
    PackageText t;
    std::ifstream f(fs::path(libraryRoot()) / pkg / kManifest);
    JsonDocument doc;
    if (!f || deserializeJson(doc, f)) return t;
    t.found = true;
    t.displayName = doc["display_name"] | "";
    t.description = doc["description"] | "";
    for (JsonVariantConst a : doc["apps"].as<JsonArrayConst>()) t.apps.emplace_back(a | "");
    return t;
}

static std::vector<uint8_t> readIconFile(const fs::path& path) {
    std::error_code ec;
    const uintmax_t size = fs::file_size(path, ec);
    if (ec || size == 0 || size > kIconBytesMax) return {};
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    std::ifstream f(path, std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()))) return {};
    return buf;
}

std::vector<uint8_t> readPackageIcon(const std::string& pkg) {
    return readIconFile(fs::path(libraryRoot()) / pkg / kIcon);
}

std::vector<uint8_t> readKitIcon(const std::string& kit) {
    return readIconFile(std::string(crosspad_gui::getGuiPlatform().assetPathPrefix()) + "fwkit_" + kit + ".bin");
}

bool setPinned(const std::string& pkg, bool pinned) {
    const fs::path flag = fs::path(libraryRoot()) / pkg / kPinned;
    std::error_code ec;
    if (pinned) {
        std::ofstream f(flag);
        return static_cast<bool>(f);
    }
    fs::remove(flag, ec);
    return !ec;
}

bool removePackage(const std::string& pkg) {
    for (const SlotRec& r : s_slots)
        if (r.package == pkg) return false;   // a slot's package stays
    std::error_code ec;
    fs::remove_all(fs::path(libraryRoot()) / pkg, ec);
    return !ec;
}

bool installAndSwitch(const std::string& pkg, bool allowUnknownBuild) {
    if (runningOnTrial()) {
        std::printf("[Firmware] install refused: this firmware has not confirmed itself yet\n");
        return false;
    }
    const fs::path src = fs::path(libraryRoot()) / pkg / kImageName;
    const ImageDesc d = readImageDesc(src.string());
    const bool ok = d.found && d.project == kProject;
    if (!ok && !(allowUnknownBuild && d.found)) {
        std::printf("[Firmware] install refused: %s is not a simulator firmware\n", src.string().c_str());
        return false;
    }
    const int slot = other(s_running);
    const std::string dst = slotImage(slot);
    std::error_code ec;
    fs::create_directories(fs::path(dst).parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::printf("[Firmware] install failed: %s\n", ec.message().c_str());
        return false;
    }
    fs::permissions(dst, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec, fs::perm_options::replace,
                    ec);
    SlotRec& r = s_slots[slot];
    r.exe = dst;
    r.package = pkg;
    r.kit = d.kit;
    r.version = d.version;
    r.state = FwSlotState::Installed;
    save();
    refreshInfo();
    std::printf("[Firmware] installed %s into slot %c\n", pkg.c_str(), label(slot));
    return switchTo(slot, true, false);
}

bool switchToOther(bool clearRollback) { return switchTo(other(s_running), clearRollback, false); }

} // namespace pc_fw
