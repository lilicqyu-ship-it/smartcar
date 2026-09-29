/*
 * ui_service.h - service pages (doc/08-architecture-v2.md §4-§7):
 *   FIRMWARE  - staged C6 / TC275 images, hold-to-update, live progress
 *   CALIBRATE - TC275 encoder direction, motor jog, calibration record
 *   DIAGNOSE  - C6 + TC275 fault diagnosis (derived findings + details)
 * All I/O is delegated to scr_svc (core 0) / scr_ctrl; these pages only
 * render snapshots and post requests.
 */
#ifndef UI_SERVICE_H
#define UI_SERVICE_H

#include "lvgl.h"
#include "ui.h"
#include "../app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_service_create(lv_obj_t *fw, lv_obj_t *calib, lv_obj_t *fdiag);

void ui_service_fw_refresh(const scr_state_t *st);
void ui_service_calib_refresh(const scr_state_t *st);
void ui_service_fdiag_refresh(const scr_state_t *st);

/* page lifecycle hooks from ui_nav_open (start/stop polling, stop jog) */
void ui_service_on_enter(ui_page_t p);
void ui_service_on_leave(ui_page_t p);

#ifdef __cplusplus
}
#endif

#endif /* UI_SERVICE_H */
