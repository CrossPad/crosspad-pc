// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file PcFwViewService.hpp
 * @brief The Firmware views in the simulator: crosspad-gui's IFwViewService
 *        over the simulator's firmware (PcFirmware.hpp) -- the packages on the
 *        virtual card, the two slots, installing and switching, which restart
 *        the simulator into the other slot's binary. The catalog stays
 *        made-up (Official and Community kits, one update waiting): a
 *        download or a check plays out over a few polls and puts nothing on
 *        the card. The platform registers it at boot, as an app that leaves
 *        the firmware (DOOM's Quit) switches through it too.
 *        LVGL thread only, as the views are.
 */

#if USE_LVGL

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "crosspad-gui/fw/IFwViewService.h"

class PcFwViewService : public crosspad_gui::IFwViewService {
public:
    PcFwViewService();

    void dataOpen() override { ++opens_; }
    void dataClose() override;
    bool dataBusy() override { return scanWanted_ || storeWanted_ || detailWanted_ || !iconsWanted_.empty(); }
    void dataPoll() override;
    void scanLibrary() override { scanWanted_ = true; }
    crosspad_gui::FwLibrarySnap* library() override { return opens_ > 0 && libReady_ ? lib_.get() : nullptr; }
    uint32_t libraryGen() override { return libGen_; }
    void loadStore() override { storeWanted_ = true; }
    void dropStore() override;
    const crosspad::FwStore* store() override { return storeLoaded_ ? &store_ : nullptr; }
    crosspad::FwStoreLoad storeState() override { return crosspad::FwStoreLoad::Ok; }
    bool storeLoaded() override { return storeLoaded_; }
    uint32_t storeGen() override { return storeGen_; }
    void loadDetail(const char* kit, const char* version, const char* pkg) override;
    const crosspad::FwKitDetail* detail() override { return detailReady_ ? &detail_ : nullptr; }
    uint32_t detailGen() override { return detailGen_; }
    void setPinned(const char* pkg, bool pinned) override;
    void remove(const char* pkg, const char* slotA, const char* slotB) override;
    void ackCommunity(const char* kit) override;
    bool communityOk(const char* kit) override;
    bool requestIcon(uint32_t ticket, const char* pkg, const char* kit, const char* version) override;
    void cancelIcon(uint32_t ticket) override;
    void freeIcon(uint8_t* buf) override;
    uint32_t libraryLimitMb() override { return kLibraryLimitMb; }

    void readSlots(crosspad::FwSlotInfo out[2]) override;
    bool runningOnTrial() override;
    /// `--boot` rolls back a slot that never confirmed itself (PcFirmware.hpp).
    bool rollbackSupported() override { return true; }
    bool installWriting() override { return false; }
    bool flowActive() override { return false; }
    bool flowInstallAndRestart(const char* pkg, bool allowUnknownBuild) override;
    bool flowSwitchAndRestart(bool clearRollback) override;
    const char* runningVersion() override;

    crosspad_gui::FwUpdaterState updaterState() override { return state_; }
    bool updaterBusy() override { return busy_; }
    uint8_t updaterProgress() override { return progress_; }
    void updaterError(char* out, size_t n) override { copy("", out, n); }
    void updaterTarget(char* out, size_t n) override { copy(target_, out, n); }
    void updaterTargetKit(char* out, size_t n) override { copy(targetKit_, out, n); }
    void updaterReadyVersion(char* out, size_t n) override { copy(ready_, out, n); }
    bool updaterChecked() override { return true; }
    int64_t updaterLastCheck() override { return store_.fetched; }
    bool channelBeta(const char*) override { return false; }
    bool checkStart(char* refusal, size_t n) override;
    bool downloadStart(const char* kit, const char* version, bool install, char* refusal, size_t n) override;

    bool wifiAcquire(const char*) override { return true; }
    void wifiRelease() override {}
    crosspad::WifiLinkState wifiState() override { return crosspad::WifiLinkState::Up; }
    crosspad::WifiFail wifiFail() override { return crosspad::WifiFail::None; }
    bool networkMode() override { return false; }
    void offerNetworkMode(crosspad_gui::FwNetTab, lv_group_t*) override {}
    int64_t nowUs() override;

    void uiHost(const char*) override {}
    void uiTab(const char* tab) override { tab_ = tab ? tab : ""; }
    bool uiTabIs(const char* tab) override { return tab_ == tab; }
    void uiItems(uint32_t) override {}
    void uiSel(int32_t, const char*, const char*) override {}
    void uiCard(const char*, const char*) override {}
    void uiStore(const char*) override {}
    void uiReset() override { tab_.clear(); }

private:
    static constexpr uint32_t kLibraryLimitMb = 512;

    static void copy(const std::string& s, char* out, size_t n);
    void scanCard();
    crosspad::FwStoreKit& addKit(const char* kit, const char* displayName, const char* author, crosspad::FwTier tier);
    void addVersion(crosspad::FwStoreKit& k, const char* version, crosspad::FwChannel channel, crosspad::FwTier tier);
    void stepJob();

    std::unique_ptr<crosspad_gui::FwLibrarySnap> lib_;
    crosspad::FwStore store_{};
    crosspad::FwKitDetail detail_{};
    int opens_ = 0;
    bool scanWanted_ = false, libReady_ = false;
    uint32_t libGen_ = 0;
    bool storeWanted_ = false, storeLoaded_ = false;
    uint32_t storeGen_ = 0;
    bool detailWanted_ = false, detailReady_ = false;
    uint32_t detailGen_ = 0;
    std::string detailKit_;
    struct IconWanted {
        uint32_t ticket;
        std::string pkg, kit;   // the package's icon.bin, else the kit's staged one
    };
    std::vector<IconWanted> iconsWanted_;

    crosspad_gui::FwUpdaterState state_ = crosspad_gui::FwUpdaterState::Available;
    bool busy_ = false;
    uint8_t progress_ = 0;
    std::string target_, targetKit_, ready_, tab_, communityOk_, runningVersion_;
};

/// The simulator's one service, registered at boot.
PcFwViewService& pcFwViewService();

#endif // USE_LVGL
