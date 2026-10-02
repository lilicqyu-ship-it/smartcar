/*
 * ui_vision.h - VISION page (Remote design doc 8.3): preview + result overlay,
 * numeric panel, vision mode / drive mode controls (CTRL gated, stale interlock).
 */
#ifndef UI_VISION_H
#define UI_VISION_H

#include "lvgl.h"
#include "../app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_vision_create(lv_obj_t *root);
void ui_vision_refresh(const scr_state_t *st);
/* 33 ms video pump (ui.c timer): newest decoded frame + result overlay
 * without waiting for the 100 ms refresh; no-op when nothing new arrived */
void ui_vision_pump_video(const scr_state_t *st);

#ifdef __cplusplus
}
#endif

#endif /* UI_VISION_H */
