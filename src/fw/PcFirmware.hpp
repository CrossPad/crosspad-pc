// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file PcFirmware.hpp
 * @brief The board's firmware library and A/B slots, played by the
 *        simulator's own binaries.
 *
 * A firmware is a simulator binary built for one kit (config/fw_kits); its
 * descriptor (PcFwDesc.cpp) says which kit and version, as the CPFI block
 * does in a board image. On the virtual card a package is
 * crosspad/firmware/<name>/ with that binary as CrossPad.pc. The two slots
 * live in the profile directory (fw/slots.json): installing copies a
 * package's binary into the slot that is not running, and switching starts
 * the other slot's binary and ends this process -- the restart the board
 * makes into its other partition.
 *
 * A binary started by hand is "flashed over USB": it is slot A, with no
 * package, and the boot slot. One started by a switch is told its slot on the
 * command line (--fw-slot=A|B). `--boot` is the board's power button and
 * bootloader: it starts whatever slot boots, and rolls back a slot that was
 * started on trial and never confirmed itself (it crashed, or was killed).
 * Holding pad 16 while the simulator boots (or --recovery) switches to the
 * slot holding the main kit, whatever runs.
 *
 * Each firmware keeps its own settings (preferences.json; the devices in
 * device_preferences.json are shared): a switch saves the running one's under
 * fw/settings/<package>.json, and a slot started by a switch takes its own
 * back before the settings are read.
 *
 * LVGL thread only, as the Firmware views that call it -- but for
 * parseArgs(), runBootloader() and boot(), which main() and the start-up run
 * before there is one.
 */

#include <string>
#include <vector>

#include "crosspad/fw/FwPackage.hpp"

namespace pc_fw {

/// The kit a recovery goes back to: the board's main firmware.
constexpr const char* kMainKit = "full";
/// The pad held at boot for a recovery (index; printed "PAD 16").
constexpr uint8_t kRecoveryPad = 15;
/// A package's binary, as CrossPad.bin is the board's.
constexpr const char* kImageName = "CrossPad.pc";

/// What a binary's descriptor says.
struct ImageDesc {
    bool found = false;
    std::string kit, version, name, project;
};

/// One package on the card.
struct Package {
    std::string name;                 ///< directory
    ImageDesc desc;
    crosspad::FwPkgVerdict verdict = crosspad::FwPkgVerdict::NoImage;
    bool complete = true;
    bool pinned = false;
    bool hasIcon = false;             ///< icon.bin
    uint64_t bytes = 0;
};

/// What a package's manifest.json says for its kit card
/// (scripts/make_fw_package.py writes it).
struct PackageText {
    bool found = false;
    std::string displayName, description;
    std::vector<std::string> apps;
};

/// The descriptor of the binary at @p path.
ImageDesc readImageDesc(const std::string& path);
/// This binary's descriptor.
ImageDesc ownDesc();

/// The command line's --fw-slot / --recovery / --boot; call from main() first.
void parseArgs(int argc, char** argv);
/// --boot: starts the slot that boots (or the one it rolls back to) and
/// returns true; main() then ends. False without --boot, or with no slot to
/// boot (this binary then starts as itself).
bool runBootloader();
/// The slot this process runs, the boot record and this firmware's own
/// settings; before pc_platform_init() reads the settings.
void boot();
/// After the boot finished: a trial slot confirms itself, and a held pad 16
/// (or --recovery) goes back to the main kit. True when a switch was started.
bool bootFinished(bool pad16Held);

void readSlots(crosspad::FwSlotInfo out[2]);
const crosspad::FwSlotInfo& runningSlot();
bool runningOnTrial();

/// crosspad/firmware on the card; empty without a card.
std::string libraryRoot();
std::vector<Package> scanLibrary();
PackageText readPackageText(const std::string& pkg);
/// <pkg>/icon.bin, the kit's 48x48 RGB565A8 icon; empty when there is none.
std::vector<uint8_t> readPackageIcon(const std::string& pkg);
/// The icon of @p kit staged with the simulator's assets (fwkit_<kit>.bin),
/// for a slot whose package is not on the card; empty when there is none.
std::vector<uint8_t> readKitIcon(const std::string& kit);
bool setPinned(const std::string& pkg, bool pinned);
bool removePackage(const std::string& pkg);

/// Copies @p pkg into the slot not running and switches to it. False (and
/// nothing changed) when refused; never returns otherwise.
bool installAndSwitch(const std::string& pkg, bool allowUnknownBuild);
/// Starts the other slot and ends this process. False when refused.
bool switchToOther(bool clearRollback);

} // namespace pc_fw
