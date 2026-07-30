// Glue between LVGL 9 and this board: display flush via the ST7735 driver, a
// 3-button "encoder" input device, the LVGL tick, and the handler task.
#pragma once

#include "lvgl.h"

// Bring LVGL up (display + input + tick + background task). Call once, after
// st7735_init() and buttons_init().
void lvgl_port_init(void);

// The input group the encoder drives. Add navigable widgets to it (or hand it
// to EEZ-generated ui code). Returns NULL before lvgl_port_init().
lv_group_t *lvgl_port_group(void);

// Guard any lv_* calls made from a task other than the LVGL task with these.
void lvgl_port_lock(void);
void lvgl_port_unlock(void);
