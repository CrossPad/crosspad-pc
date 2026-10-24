/**
 * @file    twin_usb_host.cpp
 * @brief   The board's USB host profile (Settings -> USB host), for the twin:
 *          the browser is no USB host, so nothing is ever detected and the
 *          shape in force is the mode's, Linux in Auto as on a board before
 *          enumeration. The mode lives for the session.
 */
#include "usb_host_profile.h"

static usb_host_mode_t s_mode = USB_HOST_MODE_AUTO;

void usb_host_profile_init(void) {}
void usb_host_profile_set_mode(usb_host_mode_t mode) { s_mode = mode; }
usb_host_mode_t usb_host_profile_mode(void) { return s_mode; }

usb_host_profile_t usb_host_profile_get(void)
{
    return s_mode == USB_HOST_MODE_MACOS ? USB_HOST_PROFILE_MACOS : USB_HOST_PROFILE_LINUX;
}

usb_host_seen_t usb_host_profile_detected(void) { return USB_HOST_SEEN_NONE; }

const char *usb_host_profile_name(usb_host_profile_t profile)
{
    return profile == USB_HOST_PROFILE_MACOS ? "macOS" : "Linux";
}

const char *usb_host_mode_name(usb_host_mode_t mode)
{
    return mode == USB_HOST_MODE_MACOS ? "macOS" : mode == USB_HOST_MODE_LINUX ? "Linux" : "auto";
}

const char *usb_host_seen_name(usb_host_seen_t seen)
{
    return seen == USB_HOST_SEEN_MACOS ? "macOS" : seen == USB_HOST_SEEN_LINUX ? "Linux" : "none";
}

uint32_t usb_host_enum_trace(uint32_t, uint32_t, char *out, size_t len)
{
    if (len) out[0] = '\0';
    return 0;
}
