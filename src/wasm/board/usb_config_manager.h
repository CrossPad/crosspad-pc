#pragma once
/* platform-idf main/include/usb_config_manager.h, for the twin: the board's
 * USB profile as TWIN_FW reported it. */
typedef enum { USB_MODE_DEFAULT = 0, USB_MODE_AUDIO, USB_MODE_TRANSFER, USB_MODE_COUNT } usb_mode_t;
usb_mode_t usb_config_manager_get_mode(void);
inline int usb_config_manager_set_mode(usb_mode_t) { return 0; }
