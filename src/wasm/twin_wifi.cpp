/**
 * @file    twin_wifi.cpp
 * @brief   The board's WiFi under its own WiFi page (platform-idf
 *          main/gui/wifi_page.cpp, built here unchanged), for the twin: a
 *          board whose radio is idle. The browser has no radio, and the twin
 *          link carries no SSID, signal or saved list, so the page reads Off
 *          (or "WiFi is off" when not allowed), saves nothing, a scan finds
 *          no networks (is refused when WiFi is not allowed, as on the board)
 *          and a test gets no lease. No lease is ever granted, so the link
 *          never fails and the network-mode offer never comes.
 */
#include <cstdio>

#include "board/esp_wifi_types.h"
#include "crosspad/net/WifiNetworkList.hpp"
#include "crosspad/net/WifiTypes.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "wifi_creds.h"
#include "wifi_fallback.h"
#include "wifi_link.h"
#include "wifi_ping.h"

using crosspad::WifiFail;
using crosspad::WifiLinkState;

namespace {
bool allowed()
{
    auto *cfg = crosspad::CrosspadSettings::getInstance();
    return cfg && cfg->wireless.enableWiFi;
}
} // namespace

/* wifi_link.h: no lease, no radio. A refusal's reason is the board's with
 * nothing in reach: WiFi not allowed, else no network found. */
bool wifi_link_acquire(const char *) { return false; }
void wifi_link_release(void) {}
uint8_t wifi_link_state(void) { return (uint8_t)WifiLinkState::Off; }
uint8_t wifi_link_fail(void) { return (uint8_t)(allowed() ? WifiFail::NoNetwork : WifiFail::Disabled); }
bool wifi_link_info(char *ssid, size_t n, int8_t *rssi, uint32_t *ip)
{
    if (ssid && n) ssid[0] = '\0';
    if (rssi) *rssi = 0;
    if (ip) *ip = 0;
    return false;
}
void wifi_link_connect_candidate(const char *, const char *) {}
void wifi_link_enable_changed(void) {}
int wifi_link_scan(wifi_ap_t *, int) { return allowed() ? 0 : WIFI_LINK_SCAN_REFUSED; }   // 0: "No networks found"
const char *wifi_link_auth_name(uint8_t auth)
{
    switch ((wifi_auth_mode_t)auth) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "wep";
        case WIFI_AUTH_WPA_PSK:         return "wpa";
        case WIFI_AUTH_WPA2_PSK:
        case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2e";
        case WIFI_AUTH_WPA3_PSK:        return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2wpa3";
        case WIFI_AUTH_OWE:             return "owe";
        default:                        return "other";
    }
}

/* wifi_ping.h: only ever asked under a lease, which never comes. */
int wifi_ping(uint32_t *ms, int *tlsCode)
{
    if (ms) *ms = 0;
    if (tlsCode) *tlsCode = 0;
    return 0;
}

/* wifi_creds.h: no saved networks ("None yet"); the board's list stays the board's. */
void wifi_creds_snapshot(crosspad::WifiNetworkList &out) { out = {}; }
bool wifi_creds_forget(const char *) { return false; }

/* wifi_fallback.h: no network mode to restart into. */
void wifi_fallback_offer(net_tab_t, lv_group_t *) {}
