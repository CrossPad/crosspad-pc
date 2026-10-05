#pragma once
/* platform-idf main/include/fw_library.h, for the twin: the board's package
 * library as TWIN_FW reported it (twin_board.h). */
#include <cstddef>
#include <cstdint>

#define FW_KIT_ID_LEN    16
#define FW_ASSETS_ID_LEN 32
#define FW_PKG_ROOT      "/sdcard/crosspad/firmware"
#define FW_PKG_NAME_LEN  48

enum class FwPkgVerdict : uint8_t { Ok, NoImage, NotAnImage, WrongProject, WrongBoard, TooLarge, UnknownBuild, Downloading };

struct FwPackage {
    char     name[FW_PKG_NAME_LEN];
    char     version[32];
    char     kit[FW_KIT_ID_LEN];
    uint8_t  boardRev;
    uint16_t stmProtoMin;
    uint8_t  assetsId[FW_ASSETS_ID_LEN];
    uint32_t imageSize;
    bool     hasAssets;
    bool     hasSettings;
    FwPkgVerdict verdict;
};

size_t fw_library_scan(FwPackage *out, size_t capacity);
const char *fw_pkg_verdict_text(FwPkgVerdict v);
