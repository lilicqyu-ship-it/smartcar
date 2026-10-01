/*
 * ui_alert.h - system-level full-screen alert overlay (spec 20-22).
 */
#ifndef UI_ALERT_H
#define UI_ALERT_H

#include "lvgl.h"
#include "../app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_alert_create(lv_obj_t *parent);
void ui_alert_refresh(const scr_state_t *st);

#ifdef __cplusplus
}
#endif

#endif /* UI_ALERT_H */
