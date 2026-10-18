// SPDX-License-Identifier: GPL-3.0-or-later
#if USE_LVGL

#include "PcFwViewService.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "crosspad/fw/Semver.hpp"

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
    addPackage("sampler", "1.1.0", "Sampler", FwTier::Official).pinned = true;
    addPackage("dawcontrol", "0.9.0", "DAW Control", FwTier::Official);
    addPackage("sequencer", "1.0.0", "Sequencer", FwTier::Official);
    addPackage("lofi", "0.3.0", "Lo-Fi Beats", FwTier::Community);

    const struct { char label; bool running; FwSlotState state; const char* pkg; const char* kit; const char* ver; }
        slots[] = {{'A', true, FwSlotState::Confirmed, "sampler-1.1.0", "sampler", "v1.1.0"},
                   {'B', false, FwSlotState::Installed, "dawcontrol-0.9.0", "dawcontrol", "v0.9.0"}};
    for (int i = 0; i < 2; ++i) {
        crosspad::FwSlotInfo& s = slots_[i];
        s.label = slots[i].label;
        s.running = slots[i].running;
        s.state = slots[i].state;
        std::snprintf(s.package, sizeof s.package, "%s", slots[i].pkg);
        std::snprintf(s.kit, sizeof s.kit, "%s", slots[i].kit);
        std::snprintf(s.version, sizeof s.version, "%s", slots[i].ver);
        s.hasDesc = true;
    }

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

crosspad_gui::FwLibItem& PcFwViewService::addPackage(const char* kit, const char* version, const char* displayName,
                                                     FwTier tier) {
    crosspad_gui::FwLibItem& it = lib_->items[lib_->count++];
    it = crosspad_gui::FwLibItem{};
    crosspad::fwPackageName(kit, version, it.name, sizeof it.name);
    std::snprintf(it.kit, sizeof it.kit, "%s", kit);
    std::snprintf(it.version, sizeof it.version, "%s", version);
    std::snprintf(it.displayName, sizeof it.displayName, "%s", displayName);
    it.verdict = crosspad::FwPkgVerdict::Ok;
    it.complete = true;
    it.onCard = true;
    it.community = tier == FwTier::Community;
    it.bytes = kPackageBytes;
    lib_->bytes += it.bytes;
    return it;
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
    stepJob();
}

void PcFwViewService::dropStore() {
    storeLoaded_ = false;
    ++storeGen_;
}

void PcFwViewService::loadDetail(const char* kit, const char*, const char*) {
    detail_ = crosspad::FwKitDetail{};
    const crosspad::FwStoreKit* k = crosspad::fwStoreFind(store_, kit ? kit : "");
    if (k) {
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

void PcFwViewService::setPinned(const char* pkg, bool pinned) {
    for (size_t i = 0; i < lib_->count; ++i)
        if (std::strcmp(lib_->items[i].name, pkg) == 0) lib_->items[i].pinned = pinned;
    scanWanted_ = true;
}

void PcFwViewService::remove(const char* pkg, const char* slotA, const char* slotB) {
    if (std::strcmp(pkg, slotA) == 0 || std::strcmp(pkg, slotB) == 0) return;   // a slot's package stays
    for (size_t i = 0; i < lib_->count; ++i) {
        if (std::strcmp(lib_->items[i].name, pkg) != 0) continue;
        lib_->bytes -= lib_->items[i].bytes;
        std::copy(lib_->items + i + 1, lib_->items + lib_->count, lib_->items + i);
        --lib_->count;
        break;
    }
    scanWanted_ = true;
}

void PcFwViewService::ackCommunity(const char* kit) { communityOk_ += std::string(" ") + kit + " "; }

bool PcFwViewService::communityOk(const char* kit) {
    return communityOk_.find(std::string(" ") + kit + " ") != std::string::npos;
}

void PcFwViewService::readSlots(crosspad::FwSlotInfo out[2]) {
    out[0] = slots_[0];
    out[1] = slots_[1];
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

bool PcFwViewService::downloadStart(const char* kit, const char* version, bool install, char* refusal, size_t n) {
    if (busy_) {
        copy("busy", refusal, n);
        return false;
    }
    if (!crosspad::fwStoreFind(store_, kit)) {
        copy("bad_args", refusal, n);
        return false;
    }
    busy_ = true;
    install_ = install;
    state_ = FwUpdaterState::Downloading;
    progress_ = 0;
    targetKit_ = kit;
    target_ = version;
    return true;
}

/* One step of the job in progress: a check ends where it began (the update is
 * still there), a download puts its package on the pretend card. */
void PcFwViewService::stepJob() {
    if (!busy_) return;
    progress_ = static_cast<uint8_t>(std::min<int>(progress_ + kProgressStep, kProgressDone));
    if (progress_ < kProgressDone) return;
    busy_ = false;
    progress_ = 0;
    if (state_ == FwUpdaterState::Checking) {
        state_ = FwUpdaterState::Available;
        return;
    }
    char name[crosspad::kFwPackageNameMax];
    crosspad::fwPackageName(targetKit_.c_str(), target_.c_str(), name, sizeof name);
    bool onCard = false;
    for (size_t i = 0; i < lib_->count; ++i) onCard = onCard || std::strcmp(lib_->items[i].name, name) == 0;
    if (!onCard && lib_->count < crosspad_gui::kFwLibMax) {
        const crosspad::FwStoreKit* k = crosspad::fwStoreFind(store_, targetKit_.c_str());
        addPackage(targetKit_.c_str(), target_.c_str(), k ? k->displayName : targetKit_.c_str(),
                   k ? k->tier : FwTier::Official);
    }
    scanWanted_ = true;
    state_ = install_ ? FwUpdaterState::ReadyToRestart : FwUpdaterState::Idle;
    ready_ = install_ ? target_ : "";
}

int64_t PcFwViewService::nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void PcFwViewService::copy(const std::string& s, char* out, size_t n) { std::snprintf(out, n, "%s", s.c_str()); }

#endif // USE_LVGL
