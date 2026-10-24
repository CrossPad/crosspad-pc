// SPDX-License-Identifier: GPL-3.0-or-later
#if USE_LVGL

#include "PcFwViewService.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "crosspad/fw/Semver.hpp"
#include "crosspad-gui/fw/fw_icons.h"
#include "fw/PcFirmware.hpp"

using crosspad::FwChannel;
using crosspad::FwSlotState;
using crosspad::FwTier;
using crosspad_gui::FwUpdaterState;

namespace {
constexpr uint64_t kMiB = 1024ull * 1024ull;
constexpr uint64_t kPackageBytes = 3 * kMiB;
constexpr uint8_t kProgressStep = 10;   // a job takes ten polls, a few seconds of the views' timer
constexpr uint8_t kProgressDone = 100;
constexpr int64_t kCatalogFetched = 1791763200;   // 2026-10-12
} // namespace

PcFwViewService::PcFwViewService() : lib_(new crosspad_gui::FwLibrarySnap{}) {
    store_.fetched = kCatalogFetched;
    auto& sampler = addKit("sampler", "Sampler", "CrossPad", FwTier::Official);
    addVersion(sampler, "1.0.0", FwChannel::Stable, FwTier::Official);
    addVersion(sampler, "1.1.0", FwChannel::Stable, FwTier::Official);
    addVersion(sampler, "1.2.0", FwChannel::Stable, FwTier::Official);   // the update waiting
    auto& daw = addKit("dawcontrol", "DAW Control", "CrossPad", FwTier::Official);
    addVersion(daw, "0.9.0", FwChannel::Stable, FwTier::Official);
    auto& seq = addKit("sequencer", "Sequencer", "CrossPad", FwTier::Official);
    addVersion(seq, "1.0.0", FwChannel::Stable, FwTier::Official);
    addVersion(seq, "1.1.0-beta.1", FwChannel::Beta, FwTier::Official);
    auto& lofi = addKit("lofi", "Lo-Fi Beats", "a community member", FwTier::Community);
    addVersion(lofi, "0.3.0", FwChannel::Stable, FwTier::Community);
    auto& chip = addKit("chiptune", "Chiptune", "a community member", FwTier::Community);
    addVersion(chip, "0.1.0", FwChannel::Stable, FwTier::Community);
}

/* The library from the card. Slot and running are the Library view's to set
 * (readSlots()); a package's kit, version and name come from its binary's
 * descriptor, as a board package's do from CPFI. */
void PcFwViewService::scanCard() {
    lib_->count = 0;
    lib_->bytes = 0;
    for (const pc_fw::Package& p : pc_fw::scanLibrary()) {
        if (lib_->count >= crosspad_gui::kFwLibMax) break;
        crosspad_gui::FwLibItem& it = lib_->items[lib_->count++];
        it = crosspad_gui::FwLibItem{};
        std::snprintf(it.name, sizeof it.name, "%s", p.name.c_str());
        std::snprintf(it.kit, sizeof it.kit, "%s", p.desc.kit.c_str());
        std::snprintf(it.version, sizeof it.version, "%s", p.desc.version.c_str());
        std::snprintf(it.displayName, sizeof it.displayName, "%s",
                      !p.desc.name.empty() ? p.desc.name.c_str() : p.name.c_str());
        it.verdict = p.verdict;
        it.complete = p.complete;
        it.pinned = p.pinned;
        it.icon = p.hasIcon ? crosspad_gui::FwIconSrc::Package : crosspad_gui::FwIconSrc::None;
        it.onCard = true;
        it.bytes = p.bytes;
        lib_->bytes += it.bytes;
    }
}

crosspad::FwStoreKit& PcFwViewService::addKit(const char* kit, const char* displayName, const char* author,
                                              FwTier tier) {
    crosspad::FwStoreKit& k = store_.kits[store_.kitCount++];
    k = crosspad::FwStoreKit{};
    std::snprintf(k.kit, sizeof k.kit, "%s", kit);
    std::snprintf(k.displayName, sizeof k.displayName, "%s", displayName);
    std::snprintf(k.author, sizeof k.author, "%s", author);
    k.tier = tier;
    return k;
}

void PcFwViewService::addVersion(crosspad::FwStoreKit& k, const char* version, FwChannel channel, FwTier tier) {
    crosspad::FwStoreVersion& v = k.versions[k.count++];
    std::snprintf(v.version, sizeof v.version, "%s", version);
    v.channel = channel;
    v.tier = tier;
    v.bytes = static_cast<uint32_t>(kPackageBytes);
}

void PcFwViewService::dataClose() {
    if (opens_ > 0 && --opens_ > 0) return;
    libReady_ = storeLoaded_ = detailReady_ = false;
    scanWanted_ = storeWanted_ = detailWanted_ = false;
}

void PcFwViewService::dataPoll() {
    if (scanWanted_) {
        scanWanted_ = false;
        scanCard();
        libReady_ = true;
        ++libGen_;
    }
    if (storeWanted_) {
        storeWanted_ = false;
        storeLoaded_ = true;
        ++storeGen_;
    }
    if (detailWanted_) {
        detailWanted_ = false;
        detailReady_ = true;
        ++detailGen_;
    }
    for (const IconWanted& w : std::exchange(iconsWanted_, {})) {
        const std::vector<uint8_t> icon = w.pkg.empty() ? pc_fw::readKitIcon(w.kit) : pc_fw::readPackageIcon(w.pkg);
        uint8_t* buf = icon.empty() ? nullptr : static_cast<uint8_t*>(std::malloc(icon.size()));
        if (buf) std::memcpy(buf, icon.data(), icon.size());
        fw_icons_deliver(w.ticket, buf);
    }
    stepJob();
}

void PcFwViewService::dropStore() {
    storeLoaded_ = false;
    ++storeGen_;
}

void PcFwViewService::loadDetail(const char* kit, const char*, const char* pkg) {
    detail_ = crosspad::FwKitDetail{};
    const crosspad::FwStoreKit* k = crosspad::fwStoreFind(store_, kit ? kit : "");
    const pc_fw::PackageText text = pkg && pkg[0] ? pc_fw::readPackageText(pkg) : pc_fw::PackageText{};
    if (text.found) {
        // The package's manifest, as the board reads it before the catalog.
        crosspad::fwCopyText(detail_.displayName, sizeof detail_.displayName, text.displayName.c_str());
        crosspad::fwCopyText(detail_.author, sizeof detail_.author, "CrossPad");
        crosspad::fwCopyText(detail_.description, sizeof detail_.description, text.description.c_str());
        for (const std::string& app : text.apps) {
            if (detail_.appCount >= crosspad::kFwAppsMax) break;
            crosspad::fwCopyText(detail_.apps[detail_.appCount++], crosspad::kFwAppIdMax, app.c_str());
        }
        detail_.tier = FwTier::Official;
    } else if (k) {
        crosspad::fwCopyText(detail_.displayName, sizeof detail_.displayName, k->displayName);
        crosspad::fwCopyText(detail_.author, sizeof detail_.author, k->author);
        detail_.tier = k->tier;
        std::snprintf(detail_.description, sizeof detail_.description,
                      "%s for CrossPad: kits, pads and the apps that go with them. (Simulator data.)", k->displayName);
        std::snprintf(detail_.notes, sizeof detail_.notes, "- Faster kit loading\n- Fixes in the pad mixer");
        std::snprintf(detail_.apps[0], sizeof detail_.apps[0], "sampler");
        std::snprintf(detail_.apps[1], sizeof detail_.apps[1], "pad-mixer");
        std::snprintf(detail_.apps[2], sizeof detail_.apps[2], "settings");
        detail_.appCount = 3;
    }
    detailReady_ = false;
    detailWanted_ = true;
}

bool PcFwViewService::requestIcon(uint32_t ticket, const char* pkg, const char* kit, const char*) {
    if ((!pkg || !pkg[0]) && (!kit || !kit[0])) return false;
    iconsWanted_.push_back({ticket, pkg ? pkg : "", kit ? kit : ""});
    return true;
}

void PcFwViewService::cancelIcon(uint32_t ticket) {
    iconsWanted_.erase(std::remove_if(iconsWanted_.begin(), iconsWanted_.end(),
                                      [ticket](const IconWanted& w) { return w.ticket == ticket; }),
                       iconsWanted_.end());
}

void PcFwViewService::freeIcon(uint8_t* buf) { std::free(buf); }

void PcFwViewService::setPinned(const char* pkg, bool pinned) {
    pc_fw::setPinned(pkg, pinned);
    scanWanted_ = true;
}

void PcFwViewService::remove(const char* pkg, const char* slotA, const char* slotB) {
    if (std::strcmp(pkg, slotA) != 0 && std::strcmp(pkg, slotB) != 0) pc_fw::removePackage(pkg);
    scanWanted_ = true;
}

void PcFwViewService::ackCommunity(const char* kit) { communityOk_ += std::string(" ") + kit + " "; }

bool PcFwViewService::communityOk(const char* kit) {
    return communityOk_.find(std::string(" ") + kit + " ") != std::string::npos;
}

void PcFwViewService::readSlots(crosspad::FwSlotInfo out[2]) { pc_fw::readSlots(out); }

bool PcFwViewService::runningOnTrial() { return pc_fw::runningOnTrial(); }

bool PcFwViewService::flowInstallAndRestart(const char* pkg, bool allowUnknownBuild) {
    return pc_fw::installAndSwitch(pkg ? pkg : "", allowUnknownBuild);
}

bool PcFwViewService::flowSwitchAndRestart(bool clearRollback) { return pc_fw::switchToOther(clearRollback); }

const char* PcFwViewService::runningVersion() {
    runningVersion_ = std::string("v") + pc_fw::runningSlot().version;
    return runningVersion_.c_str();
}

bool PcFwViewService::checkStart(char* refusal, size_t n) {
    if (busy_) {
        copy("busy", refusal, n);
        return false;
    }
    busy_ = true;
    state_ = FwUpdaterState::Checking;
    progress_ = 0;
    return true;
}

bool PcFwViewService::downloadStart(const char* kit, const char* version, bool, char* refusal, size_t n) {
    if (busy_) {
        copy("busy", refusal, n);
        return false;
    }
    if (!crosspad::fwStoreFind(store_, kit)) {
        copy("bad_args", refusal, n);
        return false;
    }
    busy_ = true;
    state_ = FwUpdaterState::Downloading;
    progress_ = 0;
    targetKit_ = kit;
    target_ = version;
    return true;
}

/* One step of the job in progress: a check ends where it began (the update is
 * still there); a download of the made-up catalog ends with nothing on the
 * card -- the library is the card's. */
void PcFwViewService::stepJob() {
    if (!busy_) return;
    progress_ = static_cast<uint8_t>(std::min<int>(progress_ + kProgressStep, kProgressDone));
    if (progress_ < kProgressDone) return;
    busy_ = false;
    progress_ = 0;
    state_ = state_ == FwUpdaterState::Checking ? FwUpdaterState::Available : FwUpdaterState::Idle;
    scanWanted_ = true;
}

int64_t PcFwViewService::nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void PcFwViewService::copy(const std::string& s, char* out, size_t n) { std::snprintf(out, n, "%s", s.c_str()); }

PcFwViewService& pcFwViewService() {
    static PcFwViewService service;
    return service;
}

#endif // USE_LVGL
