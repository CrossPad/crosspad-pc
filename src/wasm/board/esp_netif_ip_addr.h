#pragma once
/* ESP-IDF esp_netif_ip_addr.h, for the twin: what the WiFi page prints an
 * address with (it never has one here: twin_wifi.cpp). */
#include <cstdint>
typedef struct { uint32_t addr; } esp_ip4_addr_t;
#define esp_ip4_addr_get_byte(ipaddr, idx) (((const uint8_t *)(&(ipaddr)->addr))[idx])
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(ipaddr) esp_ip4_addr_get_byte(ipaddr, 0), esp_ip4_addr_get_byte(ipaddr, 1), \
                       esp_ip4_addr_get_byte(ipaddr, 2), esp_ip4_addr_get_byte(ipaddr, 3)
