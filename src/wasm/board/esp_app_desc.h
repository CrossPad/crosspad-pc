#pragma once
/* ESP-IDF esp_app_desc.h, for the twin: the running firmware's version as the
 * board reported it (TWIN_FW). */

#define IDF_VER "board"
typedef struct { char version[32]; char project_name[32]; char time[16]; char date[16]; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);   // twin_board.cpp
