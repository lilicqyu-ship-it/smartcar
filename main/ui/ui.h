/*
 * ui.h - page manager for the LCD UI (spec 4/5: P0 boot, P1 home drive,
 * P2 vehicle, P3 radio, P4 diagnostics, P5 settings, P6 pairing, P9 alert).
 */
#ifndef UI_H
#define UI_H

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UI_PAGE_HOME = 0,
    UI_PAGE_VEHICLE,
    UI_PAGE_RADIO,
    UI_PAGE_DIAG,
    UI_PAGE_SETTINGS,
    UI_PAGE_PAIR,
    UI_PAGE_EVENTS,
    UI_PAGE_COUNT,
} ui_page_t;

/* Called once after bsp_display_start(), in LVGL context. */
void ui_init(void);

/* Switch page (safe from LVGL event callbacks). */
void ui_nav_open(ui_page_t p);

/* Toast feedback for mode switches etc. (spec 34). */
void ui_toast(const char *fmt, ...);

/* Engineer mode (spec 86/87): off by default, unlocked on the About page. */
bool ui_engineer_mode(void);
void ui_engineer_enable(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_H */
