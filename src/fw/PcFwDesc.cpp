// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file PcFwDesc.cpp
 * @brief The simulator binary's firmware descriptor: which kit it is, as the
 *        CPFI block says in a board image. PcFirmware.cpp finds it in a file
 *        by its marker, so a package is judged without running it.
 */

/* Kept by the linker however unused: it is read from the file, never from
 * code. The fields end with ';', the block with '\n'. */
extern "C" {
#if defined(__GNUC__)
__attribute__((used))
#endif
const char crosspad_pc_fw_desc[] =
    "CPFI-PC1;kit=" CROSSPAD_FW_KIT_ID ";version=" CROSSPAD_FW_KIT_VERSION ";name=" CROSSPAD_FW_KIT_NAME
    ";project=crosspad-pc;\n";
}
