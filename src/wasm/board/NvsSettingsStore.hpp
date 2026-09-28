#pragma once
/* platform-idf's NVS store, for the twin: reads answer with the board's
 * values where the twin knows them (the OTA preference, TWIN_FW), writes are
 * the board's to make. */
#include <cstring>

#include "crosspad/settings/IKeyValueStore.hpp"
#include "twin_board.h"

class NvsSettingsStore : public crosspad::IKeyValueStore {
public:
    static NvsSettingsStore &getInstance() { static NvsSettingsStore s; return s; }
    bool init() override { return true; }
    void saveBool(const char *, const char *, bool) override {}
    void saveU8(const char *, const char *, uint8_t) override {}
    void saveI32(const char *, const char *, int32_t) override {}
    bool readBool(const char *, const char *, bool d) override { return d; }
    uint8_t readU8(const char *ns, const char *key, uint8_t d) override {
        return !strcmp(ns, "ota_prefs") && !strcmp(key, "auto_accept") ? twin_fw().autoAccept : d;
    }
    int32_t readI32(const char *, const char *, int32_t d) override { return d; }
    void eraseAll() override {}
};
