/**
 * @file    twin_fw_views.cpp
 * @brief   What the board's Firmware views (platform-idf main/gui/fw_*_view.cpp,
 *          fw_kit_card.cpp, fw_actions.cpp, fw_views_common.cpp; built here
 *          unchanged) read from the SD card, for the twin: the card is the
 *          board's, so the Library is its packages as TWIN_FW listed them and
 *          nothing is written. The Store's catalog cache, kit texts and icons
 *          stay on the board's card: the Store reads "not checked yet", a
 *          kit card shows no text, tiles keep their letter. Pins, deletes and
 *          the community acknowledgement are the board's to make.
 */
#include <cstdio>
#include <cstring>

#include "board/twin_fw.h"
#include "crosspad/fw/FwKit.hpp"
#include "crosspad/fw/FwLibraryGc.hpp"
#include "crosspad/fw/FwStore.hpp"
#include "fw_gc.h"
#include "fw_icons.h"
#include "fw_store_files.h"
#include "fw_view_data.h"
#include "fw_views_common.h"

namespace {

struct Ctx {
    bool open = false;
    int owners = 0;
    bool scanWanted = false;     // a scan was asked for: the next poll lands it
    uint32_t seenFw = 0;         // the TWIN_FW reply the last scan was made from
    FwLibrarySnap lib{};
    bool haveLib = false;
    uint32_t libGen = 0;
    bool storeWanted = false;
    bool storeLoaded = false;
    uint32_t storeGen = 0;
    crosspad::FwKitDetail detail{};
    bool detailWanted = false;
    bool haveDetail = false;
    uint32_t detailGen = 0;
    uint32_t icons[16] = {};     // tickets asked for: answered "none" on the next poll
    size_t iconCount = 0;
} s;

void scan()
{
    const TwinFw &f = twin_fw();
    FwLibrarySnap &snap = s.lib;
    char keep[sizeof snap.communityOk];
    memcpy(keep, snap.communityOk, sizeof keep);
    snap = FwLibrarySnap{};
    memcpy(snap.communityOk, keep, sizeof keep);   // what was acknowledged this session
    for (size_t i = 0; i < f.count && snap.count < kFwLibMax; ++i) {
        const FwPackage &p = f.pkgs[i];
        FwLibItem &it = snap.items[snap.count++];
        snprintf(it.name, sizeof it.name, "%s", p.name);
        snprintf(it.kit, sizeof it.kit, "%s", p.kit);
        snprintf(it.version, sizeof it.version, "%s", p.version[0] == 'v' ? p.version + 1 : p.version);
        it.verdict = p.verdict;
        it.complete = p.verdict != FwPkgVerdict::Downloading;
        it.onCard = true;
        it.stmProtoOlder = f.stmOlder[i];
        crosspad::fwCopyText(it.displayName, sizeof it.displayName, it.kit[0] ? it.kit : it.name);
        it.bytes = p.imageSize;   // TWIN_FW gives the image's size, not the directory's
        snap.bytes += it.bytes;
    }
    s.seenFw = f.gen;
    s.haveLib = true;
    ++s.libGen;
}

} // namespace

void fw_view_data_open(void)
{
    if (!s.open) s = Ctx{};
    s.open = true;
    ++s.owners;
}

void fw_view_data_close(void)
{
    if (!s.open || --s.owners > 0) return;
    s.open = false;
}

bool fw_view_data_busy(void)
{
    return s.open && (s.scanWanted || s.storeWanted || s.detailWanted || s.iconCount);
}

void fw_view_data_poll(void)
{
    if (!s.open) return;
    // A new TWIN_FW reply is what a finished card write would be on the board: a rescan.
    if (s.scanWanted || (s.haveLib && s.seenFw != twin_fw().gen)) {
        s.scanWanted = false;
        scan();
        fw_views_slots_invalidate();
    }
    if (s.storeWanted) {
        s.storeWanted = false;
        s.storeLoaded = true;
        ++s.storeGen;
    }
    if (s.detailWanted) {
        s.detailWanted = false;
        s.haveDetail = true;
        ++s.detailGen;
    }
    const size_t n = s.iconCount;
    uint32_t tickets[16];
    memcpy(tickets, s.icons, n * sizeof tickets[0]);
    s.iconCount = 0;
    for (size_t i = 0; i < n; ++i) fw_icons_deliver(tickets[i], nullptr);
}

void fw_view_data_scan_library(void) { if (s.open) s.scanWanted = true; }
FwLibrarySnap *fw_view_data_library(void) { return s.open && s.haveLib ? &s.lib : nullptr; }
uint32_t fw_view_data_library_gen(void) { return s.open ? s.libGen : 0; }

void fw_view_data_load_store(void) { if (s.open) s.storeWanted = true; }
void fw_view_data_drop_store(void)
{
    if (!s.open) return;
    s.storeLoaded = false;
    ++s.storeGen;
}
const crosspad::FwStore *fw_view_data_store(void) { return nullptr; }
crosspad::FwStoreLoad fw_view_data_store_state(void) { return crosspad::FwStoreLoad::Missing; }
bool fw_view_data_store_loaded(void) { return s.open && s.storeLoaded; }
uint32_t fw_view_data_store_gen(void) { return s.open ? s.storeGen : 0; }

void fw_view_data_load_detail(const char *kit, const char *, const char *pkg)
{
    if (!s.open) return;
    s.detail = crosspad::FwKitDetail{};
    // The package's display name as the Library shows it; the text is on the board's card.
    const char *name = kit && kit[0] ? kit : pkg ? pkg : "";
    crosspad::fwCopyText(s.detail.displayName, sizeof s.detail.displayName, name);
    s.haveDetail = false;
    s.detailWanted = true;
}
const crosspad::FwKitDetail *fw_view_data_detail(void) { return s.open && s.haveDetail ? &s.detail : nullptr; }
uint32_t fw_view_data_detail_gen(void) { return s.open ? s.detailGen : 0; }

void fw_view_data_set_pinned(const char *, bool) { fw_view_data_scan_library(); }
void fw_view_data_remove(const char *, const char *, const char *) { fw_view_data_scan_library(); }
void fw_view_data_ack_community(const char *kit)
{
    if (!s.open || !crosspad::fwKitIdValid(kit)) return;
    crosspad::fwBetaListSet(s.lib.communityOk, sizeof s.lib.communityOk, kit, true);
}
bool fw_view_data_community_ok(const char *kit)
{
    return s.open && s.haveLib && crosspad::fwBetaListHas(s.lib.communityOk, kit);
}

bool fw_view_data_request_icon(uint32_t ticket, const char *, const char *, const char *)
{
    if (!s.open || s.iconCount >= sizeof s.icons / sizeof s.icons[0]) return false;
    s.icons[s.iconCount++] = ticket;
    return true;
}

void fw_view_data_cancel_icon(uint32_t ticket)
{
    size_t kept = 0;
    for (size_t i = 0; i < s.iconCount; ++i)
        if (s.icons[i] != ticket) s.icons[kept++] = s.icons[i];
    s.iconCount = kept;
}

/* fw_store_files.h: the paths the views name; nothing is fetched or read here. */
bool fw_icon_valid(const uint8_t *, size_t) { return false; }
void fw_icon_cache_path(const char *kit, const char *version, char *out, size_t n)
{
    snprintf(out, n, "%s/%s-%s.bin", FW_ICON_DIR, kit ? kit : "", version ? version : "");
}
void fw_detail_path(const char *kit, char *out, size_t n) { snprintf(out, n, "%s/%s.json", FW_DETAIL_DIR, kit ? kit : ""); }

/* fw_gc.h: the board's card is cleaned by the board; the limit is its default
 * (the board's own setting is not in TWIN_FW). */
FwGcResult fw_gc_run(uint64_t, const char *, const char *, const char *) { return FwGcResult::Ok; }
void fw_gc_set_library_limit_mb(uint32_t) {}
uint32_t fw_gc_library_limit_mb(void) { return static_cast<uint32_t>(crosspad::kFwLibraryMaxDefault / (1024ull * 1024ull)); }
