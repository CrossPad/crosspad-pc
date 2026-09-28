#pragma once
/* ESP-IDF esp_app_desc.h, for the twin: the running firmware's version as the
 * board reported it (TWIN_FW). */
#include <cstring>
#include "twin_board.h"

#define IDF_VER "board"
typedef struct { char version[32]; char project_name[32]; char time[16]; char date[16]; } esp_app_desc_t;
inline const esp_app_desc_t *esp_app_get_description(void)
{
    static esp_app_desc_t d;
    strncpy(d.version, twin_fw().version, sizeof d.version - 1);
    return &d;
}
