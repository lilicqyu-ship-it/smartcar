/*
 * ui_pages.h - P2 Vehicle, P3 Radio, P4 Diagnostics, P5 Settings, P6 Pairing,
 * plus the engineer-mode event history (spec 83/86).
 */
#ifndef UI_PAGES_H
#define UI_PAGES_H

#include "lvgl.h"
#include "../app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_pages_create(lv_obj_t *vehicle, lv_obj_t *radio, lv_obj_t *diag,
                     lv_obj_t *settings, lv_obj_t *pair, lv_obj_t *events);

void ui_pages_vehicle_refresh(const scr_state_t *st);
void ui_pages_radio_refresh(const scr_state_t *st);
void ui_pages_diag_refresh(const scr_state_t *st);
void ui_pages_settings_refresh(const scr_state_t *st);
void ui_pages_pair_refresh(const scr_state_t *st);
void ui_pages_events_refresh(const scr_state_t *st);

/* Rebuild dynamic lists (after engineer mode toggles). */
void ui_pages_rebuild(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_PAGES_H */
