/**
 * @file    twin_board.cpp
 * @brief   The board the browser twin follows, as far as no input carries it:
 *          what its Settings and Firmware apps show (the board's own sources,
 *          built against board/*.h). The page asks the board over the
 *          companion channel (TWIN_INFO, TWIN_FW; TWIN_STATE's ble= imu=) and
 *          hands the replies in here. Nothing here drives hardware: the twin's
 *          ISettingsUI saves nothing and starts nothing, the firmware flows
 *          refuse -- the board does those, and the twin follows its screen.
 */
#include <emscripten.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "board/CrosspadPlatform.hpp"
#include "board/fw_flow_overlay.h"
#include "board/twin_board.h"
#include "board/usb_config_manager.h"
#include "crosspad/settings/ISettingsUI.hpp"

void crosspad_app_go_home();

namespace {

std::string s_bleState = "idle", s_blePeer;
uint32_t s_imuSent = 0;

/* Settings' platform half, as the board answers it (TWIN_INFO): the twin's
 * ISettingsUI shows the board's rows and starts nothing. */
struct Info {
    std::string name = "CrossPad", info;
    bool coproc = false;
    crosspad::CoprocFwInfo fw{};
    std::vector<std::pair<std::string, std::string>> rows;
} s_info;

class TwinSettingsUI : public crosspad::ISettingsUI {
public:
    void saveSettings() override {}
    void exitToHome() override { crosspad_app_go_home(); }
    const char* getPlatformName() override { return s_info.name.c_str(); }
    const char* getPlatformInfo() override { return s_info.info.empty() ? nullptr : s_info.info.c_str(); }
    bool getCoprocFwInfo(crosspad::CoprocFwInfo& out) override { out = s_info.fw; return s_info.coproc; }
    int getInfoRowCount() override { return (int)s_info.rows.size(); }
    bool getInfoRow(int i, const char** label, const char** value) override {
        if (i < 0 || i >= (int)s_info.rows.size()) return false;
        *label = s_info.rows[i].first.c_str(); *value = s_info.rows[i].second.c_str();
        return true;
    }
    const char* getBackupDir() override { return "/sdcard"; }
} s_twinUI;

/* No STM behind the twin: a sender that sends nothing, a manager with no
 * config link (Settings then shows the board's rows, from TWIN_INFO). */
class NoStm32 : public crosspad::Stm32CommandSender {
public:
    bool sendPowerOff() override { return false; }
    bool sendBrightness(uint8_t) override { return false; }
    bool sendKeepAlive() override { return false; }
    bool sendDfuRequest(bool) override { return false; }
    bool sendRoutingMatrix(const crosspad::STMRoutingSettings&) override { return false; }
};

} // namespace

namespace crosspad {
Stm32CommandSender& getStm32CommandSender() { static NoStm32 s; return s; }
Stm32Manager& getStm32Manager() { static Stm32Manager m; return m; }
} // namespace crosspad

TwinFw& twin_fw() { static TwinFw f; return f; }

void fw_slots_get(FwSlotInfo out[2]) { out[0] = twin_fw().slots[0]; out[1] = twin_fw().slots[1]; }
bool fw_rollback_supported(void) { return twin_fw().rollback; }
size_t fw_library_scan(FwPackage* out, size_t capacity)
{
    const TwinFw& f = twin_fw();
    const size_t n = f.count < capacity ? f.count : capacity;
    for (size_t i = 0; i < n; ++i) out[i] = f.pkgs[i];
    return n;
}
bool fw_package_stm_proto_older(const FwPackage& pkg)
{
    const TwinFw& f = twin_fw();
    for (size_t i = 0; i < f.count; ++i) if (!strcmp(f.pkgs[i].name, pkg.name)) return f.stmOlder[i];
    return false;
}
const char* fw_pkg_verdict_text(FwPkgVerdict v)
{
    switch (v) {
        case FwPkgVerdict::Ok:           return "ok";
        case FwPkgVerdict::NoImage:      return "no CrossPad.bin";
        case FwPkgVerdict::NotAnImage:   return "not a firmware image";
        case FwPkgVerdict::WrongProject: return "another project";
        case FwPkgVerdict::WrongBoard:   return "built for another board revision";
        case FwPkgVerdict::TooLarge:     return "too large for the slot";
        case FwPkgVerdict::UnknownBuild: return "built before packages existed";
    }
    return "unknown";
}
bool fw_flow_active(void) { return twin_fw().flow; }
usb_mode_t usb_config_manager_get_mode(void) { return twin_fw().usbDefault ? USB_MODE_DEFAULT : USB_MODE_AUDIO; }

const char* twin_ble_state() { return s_bleState.c_str(); }
const char* twin_ble_peer() { return s_blePeer.c_str(); }
uint32_t twin_imu_sent() { return s_imuSent; }
void twin_board_set_ble(const char* state, const char* peer)
{
    s_bleState = state ? state : "idle";
    s_blePeer = peer && strcmp(peer, "-") ? peer : "";
}
void twin_board_set_imu(uint32_t sent) { s_imuSent = sent; }
void twin_board_attach() { crosspad::setSettingsUI(&s_twinUI); }

extern "C" {

/** The board's TWIN_INFO replies, one per line ("TWIN_INFO P …", "… C …", "… <n> <i> …"). */
EMSCRIPTEN_KEEPALIVE void wasm_twin_info(const char* text) {
    if (!text) return;
    Info in;
    const char* p = text;
    while (*p) {
        const char* e = strchr(p, '\n');
        std::string line(p, e ? (size_t)(e - p) : strlen(p));
        p = e ? e + 1 : p + line.size();
        if (line.compare(0, 10, "TWIN_INFO ") != 0) continue;
        std::string rest = line.substr(10);
        auto split = [](const std::string& v) {
            std::vector<std::string> f;
            size_t a = 0;
            for (;;) { const size_t t = v.find('\t', a); f.push_back(v.substr(a, t == std::string::npos ? std::string::npos : t - a)); if (t == std::string::npos) break; a = t + 1; }
            return f;
        };
        if (rest[0] == 'P') { auto f = split(rest.substr(2)); in.name = f[0]; in.info = f.size() > 1 ? f[1] : ""; }
        else if (rest[0] == 'C') {
            auto f = split(rest.substr(2));
            if (f.size() >= 5) {
                in.coproc = f[0] == "1";
                snprintf(in.fw.running, sizeof in.fw.running, "%s", f[1].c_str());
                snprintf(in.fw.bundled, sizeof in.fw.bundled, "%s", f[2].c_str());
                in.fw.due = f[3] == "1"; in.fw.canUpdate = f[4] == "1";
            }
        } else {
            const size_t s1 = rest.find(' '), s2 = rest.find(' ', s1 + 1);
            if (s2 == std::string::npos) continue;
            auto f = split(rest.substr(s2 + 1));
            in.rows.emplace_back(f[0], f.size() > 1 ? f[1] : "");
        }
    }
    s_info = in;
}

/** The board's TWIN_FW replies (M, S 0, S 1, L 0..n-1), one per line. */
EMSCRIPTEN_KEEPALIVE void wasm_twin_fw(const char* text) {
    if (!text) return;
    TwinFw f;
    const char* p = text;
    while (*p) {
        const char* e = strchr(p, '\n');
        std::string line(p, e ? (size_t)(e - p) : strlen(p));
        p = e ? e + 1 : p + line.size();
        if (line.compare(0, 8, "TWIN_FW ") != 0) continue;
        const char* l = line.c_str() + 8;
        auto tabs = [](const char* from) {
            std::vector<std::string> v;
            std::string cur;
            for (const char* q = from; *q; ++q) { if (*q == '\t') { v.push_back(cur); cur.clear(); } else cur += *q; }
            v.push_back(cur);
            return v;
        };
        if (*l == 'M') {
            int rb = 0, ud = 0, aa = 0, fl = 0;
            char ver[40] = "";
            if (sscanf(l + 2, "%d %d %d %d %39s", &rb, &ud, &aa, &fl, ver) >= 4) {
                f.rollback = rb; f.usbDefault = ud; f.autoAccept = aa; f.flow = fl;
                snprintf(f.version, sizeof f.version, "%s", ver);
            }
        } else if (*l == 'S') {
            int i = 0, run = 0, st = 0, desc = 0, used = 0;
            if (sscanf(l + 2, "%d %d %d %d %n", &i, &run, &st, &desc, &used) >= 4 && used && i >= 0 && i < 2) {
                auto v = tabs(l + 2 + used);
                FwSlotInfo& s = f.slots[i];
                s.running = run; s.state = (FwSlotState)st; s.hasDesc = desc;
                s.label = v[0].empty() ? (i ? 'B' : 'A') : v[0][0];
                snprintf(s.package, sizeof s.package, "%s", v.size() > 1 ? v[1].c_str() : "");
                snprintf(s.version, sizeof s.version, "%s", v.size() > 2 ? v[2].c_str() : "");
                snprintf(s.kit, sizeof s.kit, "%s", v.size() > 3 ? v[3].c_str() : "");
            }
        } else if (*l == 'L') {
            unsigned n = 0, rev = 0, proto = 0; int i = 0, verdict = 0, as = 0, se = 0, older = 0, used = 0;
            unsigned long size = 0;
            const int got = sscanf(l + 2, "%u %d %d %u %u %lu %d %d %d %n", &n, &i, &verdict, &rev, &proto, &size, &as, &se,
                                   &older, &used);
            f.count = n < 16 ? n : 16;
            if (got >= 9 && used && i >= 0 && i < 16) {
                auto v = tabs(l + 2 + used);
                FwPackage& k = f.pkgs[i];
                k = FwPackage{};
                snprintf(k.name, sizeof k.name, "%s", v[0].c_str());
                snprintf(k.version, sizeof k.version, "%s", v.size() > 1 ? v[1].c_str() : "");
                snprintf(k.kit, sizeof k.kit, "%s", v.size() > 2 ? v[2].c_str() : "");
                k.boardRev = (uint8_t)rev; k.stmProtoMin = (uint16_t)proto; k.imageSize = (uint32_t)size;
                k.hasAssets = as; k.hasSettings = se; k.verdict = (FwPkgVerdict)verdict;
                f.stmOlder[i] = older;
            }
        }
    }
    twin_fw() = f;
}

} // extern "C"
