// Demo settings UI built with LVGL widgets, navigable with the 3-button encoder.
// This is the hand-coded reference; replace/augment with EEZ Studio-generated
// screens (see README) by adding their widgets to lvgl_port_group().
#pragma once

// Build the settings screen on the active display. Call once, after
// lvgl_port_init(), while holding the LVGL lock.
void ui_menu_start(void);
