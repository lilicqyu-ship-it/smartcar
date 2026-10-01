/*
 * ui_joystick.h - virtual joystick widget (spec 13-15).
 *
 * Outer ring + inner knob + centre point.  Radial dead zone (configurable,
 * spec 15), clamp at full travel, visual snap-back on release, live
 * Throttle/Steering values rendered by the Home page from app_state.
 */
#ifndef UI_JOYSTICK_H
#define UI_JOYSTICK_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *ui_joystick_create(lv_obj_t *parent, int size);

/*
 * Live-input gate, refreshed at 10 Hz from the Home page: enabled only while
 * the S3 holds control on a connected link (spec 104).  Disabling zeroes the
 * input, snaps the knob to centre and dims the widget.
 */
void ui_joystick_set_enabled(bool en);

#ifdef __cplusplus
}
#endif

#endif /* UI_JOYSTICK_H */
