#pragma once
/**
 * @file twin_state.h
 * @brief What the browser twin checks itself against (companion TWIN_STATE).
 *
 * The same header is compiled into the board and into the twin (brag-output's
 * wasm/sync.sh copies it), so both sides describe their screens with the same
 * code: the object tree under the LCD's root (the parent of the app
 * container), the encoder group's focus, the scroll offsets, and the overlay
 * layer. Only structure goes in the hash — classes, the hidden flag, child
 * counts — never text, which carries the board's clock and battery. The
 * status bar is reduced to its hidden flag for the same reason: its icons
 * show radios and power the twin does not have.
 *
 *   hash=<8 hex> geo=<8 hex> ovl=<8 hex> focus=<path>[e] scroll=<path>:<x>:<y>,...
 *
 * A path is r (the LCD root) or o (the overlay parent) followed by child
 * indices: r1.0.3. "-" when there is none. "e" marks a group in edit mode.
 * geo hashes the objects' coordinates relative to the root: it tells a layout
 * or scroll difference apart from a structural one, but it also moves with
 * every running animation, so it is only ever compared on a still screen.
 * Call with the LVGL lock held.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>

#include "lvgl.h"
#include "lvgl_private.h"   /* lv_obj_class_t::name */
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/settings/IKeyValueStore.hpp"

namespace twin_state {

inline uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    return h;
}

inline uint32_t fnv_str(uint32_t h, const char *s) { return s ? fnv(h, s, strlen(s) + 1) : fnv(h, "", 1); }

struct Walk {
    lv_obj_t *root;
    lv_obj_t *keep;     /* the one child of root walked in full (the app container) */
    int32_t   ox, oy;   /* root's origin, for geo */
    uint32_t  hash, geo;
    char     *scroll;   /* "path:x:y," list being built */
    size_t    scrollCap, scrollLen;
    unsigned  scrollN;
};

/* r1.0.3 — false when obj is not under base. */
inline bool path_of(lv_obj_t *obj, lv_obj_t *base, char tag, char *out, size_t cap)
{
    int32_t idx[24];
    int n = 0;
    lv_obj_t *o = obj;
    while (o && o != base && n < 24) { idx[n++] = lv_obj_get_index(o); o = lv_obj_get_parent(o); }
    if (o != base) return false;
    size_t len = (size_t)snprintf(out, cap, "%c", tag);
    for (int i = n - 1; i >= 0 && len < cap; --i)
        len += (size_t)snprintf(out + len, cap - len, i == n - 1 ? "%ld" : ".%ld", (long)idx[i]);
    return len < cap;
}

inline lv_obj_t *obj_at(lv_obj_t *root, lv_obj_t *overlay, const char *path)
{
    if (!path || !*path || *path == '-') return nullptr;
    lv_obj_t *o = *path == 'r' ? root : *path == 'o' ? overlay : nullptr;
    const char *p = path + 1;
    while (o && *p && *p != 'e' && *p != ' ') {
        char *end = nullptr;
        const long i = strtol(p, &end, 10);
        if (end == p) return nullptr;
        o = (i >= 0 && (uint32_t)i < lv_obj_get_child_count(o)) ? lv_obj_get_child(o, (int32_t)i) : nullptr;
        p = *end == '.' ? end + 1 : end;
    }
    return o;
}

inline void walk(Walk &w, lv_obj_t *obj, char tag, lv_obj_t *base, bool deep)
{
    const lv_obj_class_t *cls = lv_obj_get_class(obj);
    const uint8_t hidden = lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) ? 1 : 0;
    w.hash = fnv_str(w.hash, cls ? cls->name : "?");
    w.hash = fnv(w.hash, &hidden, 1);
    if (!deep) return;
    const uint32_t n = lv_obj_get_child_count(obj);
    w.hash = fnv(w.hash, &n, sizeof n);
    if (!hidden) {
        lv_area_t a;
        lv_obj_get_coords(obj, &a);
        const int32_t g[4] = {a.x1 - w.ox, a.y1 - w.oy, a.x2 - w.ox, a.y2 - w.oy};
        w.geo = fnv(w.geo, g, sizeof g);
        const int32_t sx = lv_obj_get_scroll_x(obj), sy = lv_obj_get_scroll_y(obj);
        if ((sx || sy) && w.scrollN < 6 && w.scrollCap > w.scrollLen + 48) {
            char p[64];
            if (path_of(obj, base, tag, p, sizeof p)) {
                w.scrollLen += (size_t)snprintf(w.scroll + w.scrollLen, w.scrollCap - w.scrollLen, "%s%s:%ld:%ld",
                                                w.scrollN ? "," : "", p, (long)sx, (long)sy);
                ++w.scrollN;
            }
        }
    }
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t *c = lv_obj_get_child(obj, (int32_t)i);
        walk(w, c, tag, base, obj != w.root || c == w.keep);
    }
}

/**
 * @param app_c   the app container (its parent is the LCD root)
 * @param overlay crosspad_gui::getOverlayParent()
 * @param group   the navigation encoder's group
 */
inline size_t describe(char *out, size_t cap, lv_obj_t *app_c, lv_obj_t *overlay, lv_group_t *group)
{
    lv_obj_t *root = app_c ? lv_obj_get_parent(app_c) : nullptr;
    if (!root || cap < 64) return (size_t)snprintf(out, cap, "hash=0 geo=0 ovl=0 focus=- scroll=-");
    static char scroll[200];
    scroll[0] = 0;
    lv_area_t ra;
    lv_obj_get_coords(root, &ra);
    Walk w{root, app_c, ra.x1, ra.y1, 2166136261u, 2166136261u, scroll, sizeof scroll, 0, 0};
    walk(w, root, 'r', root, true);
    const uint32_t hash = w.hash, geo = w.geo;

    /* The overlay: structure only; its scrolls go on the same list. */
    w.hash = 2166136261u;
    w.root = nullptr;
    if (overlay) {
        lv_area_t oa;
        lv_obj_get_coords(overlay, &oa);
        w.ox = oa.x1; w.oy = oa.y1;
        walk(w, overlay, 'o', overlay, true);
    }
    const uint32_t ovl = w.hash;

    char focus[72] = "-";
    lv_obj_t *f = group ? lv_group_get_focused(group) : nullptr;
    if (f && !path_of(f, root, 'r', focus, sizeof focus - 1) && !(overlay && path_of(f, overlay, 'o', focus, sizeof focus - 1)))
        strcpy(focus, "?");
    if (f && group && lv_group_get_editing(group)) strcat(focus, "e");

    return (size_t)snprintf(out, cap, "hash=%08lx geo=%08lx ovl=%08lx focus=%s scroll=%s",
                            (unsigned long)hash, (unsigned long)geo, (unsigned long)ovl, focus,
                            scroll[0] ? scroll : "-");
}

/* The settings as NVS holds them, through the settings' own saveTo()/loadFrom():
 * "ns/key=value;" per field, in saveTo()'s order. Portable (no struct layout),
 * and exactly what persists. The hash says whether two sides agree. */
struct SettingsText : crosspad::IKeyValueStore {
    std::string text;
    std::map<std::string, int32_t> in;
    bool init() override { return true; }
    void put(const char *ns, const char *key, long v) {
        text += ns; text += '/'; text += key; text += '='; text += std::to_string(v); text += ';';
    }
    void saveBool(const char *ns, const char *key, bool v) override { put(ns, key, v ? 1 : 0); }
    void saveU8(const char *ns, const char *key, uint8_t v) override { put(ns, key, v); }
    void saveI32(const char *ns, const char *key, int32_t v) override { put(ns, key, v); }
    int32_t get(const char *ns, const char *key, int32_t d) {
        auto it = in.find(std::string(ns) + '/' + key);
        return it == in.end() ? d : it->second;
    }
    bool readBool(const char *ns, const char *key, bool d) override { return get(ns, key, d ? 1 : 0) != 0; }
    uint8_t readU8(const char *ns, const char *key, uint8_t d) override { return (uint8_t)get(ns, key, d); }
    int32_t readI32(const char *ns, const char *key, int32_t d) override { return get(ns, key, d); }
    void eraseAll() override { in.clear(); }
};

inline std::string settings_text(crosspad::CrosspadSettings &s)
{
    SettingsText t;
    s.saveTo(t);
    return t.text;
}

inline uint32_t settings_hash(crosspad::CrosspadSettings &s)
{
    const std::string t = settings_text(s);
    return fnv(2166136261u, t.data(), t.size());
}

/** Load a settings_text() from the other side; false when it did not parse. */
inline bool settings_apply(crosspad::CrosspadSettings &s, const std::string &text)
{
    SettingsText t;
    size_t at = 0;
    while (at < text.size()) {
        const size_t eq = text.find('=', at), end = text.find(';', at);
        if (eq == std::string::npos || end == std::string::npos || eq > end) return false;
        t.in[text.substr(at, eq - at)] = (int32_t)strtol(text.c_str() + eq + 1, nullptr, 10);
        at = end + 1;
    }
    s.loadFrom(t);
    return true;
}

} // namespace twin_state
