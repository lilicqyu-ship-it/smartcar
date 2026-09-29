/*
 * ui_home.h - P1 drive page (spec 8-19, 70, 110).
 *
 * Responsive flex layout (spec 3): built from proportions of the actual
 * display resolution, verified on the 800x480 SUB3 of the EV-Board-2 kit and
 * structurally valid on 480x480 as well.
 */
#ifndef UI_HOME_H
#define UI_HOME_H

#include "lvgl.h"
#include "../app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_home_create(lv_obj_t *root);
void ui_home_refresh(const scr_state_t *st);

#ifdef __cplusplus
}
#endif

#endif /* UI_HOME_H */
