#include "ui_joystick.h"
#include "ui_theme.h"

#include <math.h>
#include <string.h>

#include "../app_state.h"
#include "../scr_settings.h"
#include "../scr_ctrl.h"

#define JOY_KNOB_PCT     34   /* knob diameter, % of widget size      */
#define JOY_MARGIN_PX    6    /* knob centre travel margin            */

typedef struct {
    lv_obj_t   *pad;
    lv_obj_t   *knob;
    lv_obj_t   *dot;
    int         size;
    int         knob_r;
    int         max_r;      /* max knob centre offset from pad centre */
    uint8_t     deadzone;   /* % of full radius, captured on press    */
    int         last_dx, last_dy;   /* knob offset at release, for snap-back */
    bool        enabled;    /* live input gate (conn + owner checked) */
    int         drawn_dx, drawn_dy; /* last position painted (2 px hysteresis) */
    bool        drawn_valid;
} joy_t;

static joy_t s_joy;

static void knob_to(lv_obj_t *knob, int x, int y)
{
    lv_obj_set_pos(knob, s_joy.size / 2 + x - s_joy.knob_r,
                   s_joy.size / 2 + y - s_joy.knob_r);
}

static void knob_center(lv_obj_t *knob)
{
    knob_to(knob, 0, 0);
}

static void apply_dim(bool dim)
{
    /* HUD ring: cyan outline while live, dim grid line while locked - the
     * state change is also carried by the knob colour and the header text */
    lv_color_t pad_border = dim ? lv_color_hex(UI_COL_LINE)
                                : lv_color_hex(UI_COL_ACCENT);
    lv_color_t knob_col = dim ? lv_color_hex(UI_COL_SURFACE2)
                              : lv_color_hex(UI_COL_ACCENT);
    lv_obj_set_style_border_color(s_joy.pad, pad_border, 0);
    lv_obj_set_style_border_opa(s_joy.pad, dim ? LV_OPA_COVER : LV_OPA_70, 0);
    lv_obj_set_style_bg_color(s_joy.knob, knob_col, 0);
    lv_obj_set_style_border_color(s_joy.knob,
                                  dim ? lv_color_hex(UI_COL_LINE)
                                      : lv_color_hex(0xB8F3FF), 0);
    lv_obj_set_style_text_color(s_joy.dot,
                                dim ? lv_color_hex(UI_COL_SURFACE2)
                                    : lv_color_hex(UI_COL_DIM), 0);
}

static void snapback_anim(void *var, int32_t v)
{
    lv_obj_t *knob = var;
    knob_to(knob, (int)((int64_t)s_joy.last_dx * (1000 - v) / 1000),
                 (int)((int64_t)s_joy.last_dy * (1000 - v) / 1000));
}

static void joy_release(void)
{
    s_joy.drawn_valid = false;
    /* visual snap back (spec 13: 视觉回弹) */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_joy.knob);
    lv_anim_set_exec_cb(&a, snapback_anim);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    app_state_set_joy(0, 0);
    scr_ctrl_kick();        /* stop now, not on the next tick */
}

static void joy_pressing(lv_event_t *e)
{
    /* failsafe: without a live CTRL link the stick is inert - it must not
     * resurrect a stale deflection when the link comes back with a finger
     * still resting on the pad (spec 104) */
    if (!s_joy.enabled) {
        return;
    }

    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_area_t a;
    lv_obj_get_coords(s_joy.pad, &a);
    int cx = (a.x1 + a.x2) / 2;
    int cy = (a.y1 + a.y2) / 2;

    float dx = (float)(p.x - cx);
    float dy = (float)(p.y - cy);
    float len = sqrtf(dx * dx + dy * dy);
    float r = len > 0.0f ? len : 0.0f;
    if (r > (float)s_joy.max_r) {
        float k = (float)s_joy.max_r / r;
        dx *= k;
        dy *= k;
        r = (float)s_joy.max_r;
    }

    s_joy.last_dx = (int)dx;
    s_joy.last_dy = (int)dy;
    if (!s_joy.drawn_valid ||
        (int)dx - s_joy.drawn_dx > 1 || s_joy.drawn_dx - (int)dx > 1 ||
        (int)dy - s_joy.drawn_dy > 1 || s_joy.drawn_dy - (int)dy > 1) {
        s_joy.drawn_dx = (int)dx;
        s_joy.drawn_dy = (int)dy;
        s_joy.drawn_valid = true;
        knob_to(s_joy.knob, (int)dx, (int)dy);
    }

    /* radial dead zone with linear remap from its edge (spec 15) */
    float dz = (float)s_joy.deadzone / 100.0f;
    float scale = 0.0f;
    float norm = r / (float)s_joy.max_r;
    if (norm > dz) {
        scale = (norm - dz) / (1.0f - dz);
    }

    int16_t v = 0, w = 0;
    if (scale > 0.0f && r > 0.0f) {
        v = (int16_t)(-dy / r * scale * SCR_DRIVE_V_MAX);   /* up = forward */
        w = (int16_t)(-dx / r * scale * SCR_DRIVE_W_MAX);   /* right = right turn */
    }
    app_state_set_joy(v, w);
    /* only a changed command wakes the ctrl task (sub-dead-zone jitter and
     * the 2 px hysteresis never generate extra frames) */
    static int16_t last_v, last_w;
    if (v != last_v || w != last_w) {
        last_v = v;
        last_w = w;
        scr_ctrl_kick();
    }
}

static void joy_pressed(lv_event_t *e)
{
    if (!s_joy.enabled) {
        return;
    }
    scr_settings_t set;
    scr_settings_get(&set);
    s_joy.deadzone = set.deadzone_pct;

    /* touching the stick is the explicit re-engage after STOP (spec 19/105) */
    scr_ctrl_joystick_touch();
    joy_pressing(e);
}

static void joy_released(lv_event_t *e)
{
    (void)e;
    if (!s_joy.enabled) {
        return;
    }
    joy_release();
}

lv_obj_t *ui_joystick_create(lv_obj_t *parent, int size)
{
    memset(&s_joy, 0, sizeof(s_joy));
    s_joy.size = size;
    s_joy.knob_r = size * JOY_KNOB_PCT / 200;
    s_joy.max_r = size / 2 - s_joy.knob_r - JOY_MARGIN_PX;
    s_joy.enabled = false;      /* Home page enables it once the link is up */

    /* pad: outer ring */
    s_joy.pad = lv_obj_create(parent);
    lv_obj_set_size(s_joy.pad, size, size);
    lv_obj_set_style_radius(s_joy.pad, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_joy.pad, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_joy.pad, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_joy.pad, lv_color_hex(UI_COL_SURFACE2), 0);
    lv_obj_set_style_border_width(s_joy.pad, 2, 0);
    lv_obj_set_style_pad_all(s_joy.pad, 0, 0);
    lv_obj_remove_flag(s_joy.pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_joy.pad, LV_OBJ_FLAG_CLICKABLE);

    /* inner range ring (max deflection) - static, drawn once */
    lv_obj_t *ring = lv_obj_create(s_joy.pad);
    int rs = s_joy.max_r * 2;
    lv_obj_set_size(ring, rs, rs);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, lv_color_hex(UI_COL_LINE), 0);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);

    /* fine cross guides */
    static const char *marks[4] = { LV_SYMBOL_UP, LV_SYMBOL_DOWN,
                                    LV_SYMBOL_LEFT, LV_SYMBOL_RIGHT };
    for (int i = 0; i < 4; i++) {
        lv_obj_t *m = lv_label_create(s_joy.pad);
        lv_label_set_text(m, marks[i]);
        lv_obj_set_style_text_color(m, lv_color_hex(UI_COL_SURFACE2), 0);
        lv_obj_set_style_text_font(m, F_MD, 0);
        switch (i) {
            case 0: lv_obj_align(m, LV_ALIGN_TOP_MID, 0, 10); break;
            case 1: lv_obj_align(m, LV_ALIGN_BOTTOM_MID, 0, -10); break;
            case 2: lv_obj_align(m, LV_ALIGN_LEFT_MID, 14, 0); break;
            case 3: lv_obj_align(m, LV_ALIGN_RIGHT_MID, -14, 0); break;
        }
    }

    /* centre dot */
    s_joy.dot = lv_obj_create(s_joy.pad);
    lv_obj_set_size(s_joy.dot, 8, 8);
    lv_obj_set_style_radius(s_joy.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_joy.dot, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_set_style_bg_opa(s_joy.dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_joy.dot, 0, 0);
    lv_obj_align(s_joy.dot, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(s_joy.dot, LV_OBJ_FLAG_CLICKABLE);

    /* knob */
    s_joy.knob = lv_obj_create(s_joy.pad);
    int ks = s_joy.knob_r * 2;
    lv_obj_set_size(s_joy.knob, ks, ks);
    lv_obj_set_style_radius(s_joy.knob, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_joy.knob, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_joy.knob, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_joy.knob, 0, 0);
    lv_obj_set_style_border_width(s_joy.knob, 3, 0);
    lv_obj_remove_flag(s_joy.knob, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_joy.knob, LV_OBJ_FLAG_SCROLLABLE);
    knob_center(s_joy.knob);

    apply_dim(true);
    lv_obj_add_event_cb(s_joy.pad, joy_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_joy.pad, joy_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_joy.pad, joy_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_joy.pad, joy_released, LV_EVENT_PRESS_LOST, NULL);

    return s_joy.pad;
}

/*
 * 10 Hz from the Home page: gate live input on connection + control owner,
 * force the stick back to centre when the gate closes (mid-drag link loss
 * must not linger as a drive command once the link recovers), and mirror the
 * gate in the visuals - dimmed = not drivable.
 */
void ui_joystick_set_enabled(bool en)
{
    if (en == s_joy.enabled) {
        return;
    }
    s_joy.enabled = en;
    apply_dim(!en);
    if (!en) {
        s_joy.last_dx = 0;
        s_joy.last_dy = 0;
        s_joy.drawn_valid = false;
        knob_center(s_joy.knob);
        app_state_set_joy(0, 0);
    }
}
