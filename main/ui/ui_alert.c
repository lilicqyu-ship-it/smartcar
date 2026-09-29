/*
 * ui_alert.c - system-level Alert Overlay (spec 20-22, 63).
 *
 * Full screen, only for safety-relevant events.  INFO/NOTICE never get here -
 * they stay in the status bar.  Emergency stop demands the explicit RELEASE;
 * everything else offers ACKNOWLEDGE.
 */
#include "ui_alert.h"
#include "ui_theme.h"
#include "ui.h"

#include "../scr_ctrl.h"
#include "../app_state.h"

static lv_obj_t *s_ov;
static lv_obj_t *s_icon;
static lv_obj_t *s_title;
static lv_obj_t *s_msg;
static lv_obj_t *s_btn;
static lv_obj_t *s_btn_lbl;
static uint32_t  s_cur_id;
static bool      s_shown;       /* z-order / visibility ops only on the
                                 * transition: repeating them every 10 Hz tick
                                 * invalidates this full-screen object and the
                                 * DIRECT_MODE repaint strobes the panel */

static void btn_cb(lv_event_t *e)
{
    if (s_cur_id == SCR_ALERT_ID_EMERGENCY) {
        scr_ctrl_emergency_release();
    } else {
        app_alert_ack();
    }
}

void ui_alert_create(lv_obj_t *parent)
{
    s_ov = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_ov, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_ov, lv_color_hex(UI_COL_CRIT), 0);
    lv_obj_set_style_bg_opa(s_ov, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_ov, 0, 0);
    lv_obj_set_style_radius(s_ov, 0, 0);
    lv_obj_remove_flag(s_ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ov, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_color(s_ov, lv_color_hex(UI_COL_CRIT), 0);

    /* red border frame while active (spec 63) */
    lv_obj_set_style_border_width(s_ov, 6, 0);

    s_icon = lv_label_create(s_ov);
    lv_obj_set_style_text_font(s_icon, F_XXL, 0);
    lv_obj_align(s_icon, LV_ALIGN_TOP_MID, 0, 90);

    s_title = lv_label_create(s_ov);
    lv_obj_set_style_text_font(s_title, F_XL, 0);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 170);

    s_msg = lv_label_create(s_ov);
    lv_obj_set_width(s_msg, 380);
    ui_label_set_color(s_msg, lv_color_hex(UI_COL_TXT));
    lv_obj_set_style_text_font(s_msg, F_MD, 0);
    lv_obj_set_style_text_align(s_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_msg, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_msg, LV_ALIGN_TOP_MID, 0, 215);

    s_btn = lv_button_create(s_ov);
    lv_obj_set_size(s_btn, 240, 56);
    lv_obj_align(s_btn, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_btn, LV_OPA_20, 0);
    lv_obj_set_style_radius(s_btn, 14, 0);
    lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_CLICKED, NULL);

    s_btn_lbl = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_btn_lbl, F_LG, 0);
    lv_obj_center(s_btn_lbl);
}

void ui_alert_refresh(const scr_state_t *st)
{
    bool show = st->boot_sys && st->alert.level != SCR_ALERT_NONE && !st->alert_ack;

    if (!show) {
        if (s_shown) {
            s_shown = false;
            lv_obj_add_flag(s_ov, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    static lv_color_t last_bg;
    lv_color_t bg;
    switch (st->alert.level) {
        case SCR_ALERT_CRITICAL: bg = lv_color_hex(UI_COL_CRIT);  break;
        case SCR_ALERT_WARNING:  bg = lv_color_hex(UI_COL_WARN);  break;
        case SCR_ALERT_NOTICE:   bg = lv_color_hex(UI_COL_INFO);  break;
        default:                 bg = lv_color_hex(UI_COL_SURFACE2); break;
    }
    if (!lv_color_eq(last_bg, bg)) {
        last_bg = bg;
        lv_obj_set_style_bg_color(s_ov, bg, 0);
        lv_obj_set_style_border_color(s_ov, bg, 0);
    }

    ui_label_set_color(s_icon, lv_color_hex(UI_COL_TXT));
    ui_label_set_text(s_icon, st->alert.level == SCR_ALERT_CRITICAL
                                  ? LV_SYMBOL_WARNING
                                  : LV_SYMBOL_BELL);
    ui_label_set_text(s_title, st->alert.title);
    ui_label_set_color(s_title, lv_color_hex(UI_COL_TXT));
    ui_label_set_text(s_msg, st->alert.msg);

    s_cur_id = st->alert.id;
    ui_label_set_text(s_btn_lbl,
                      s_cur_id == SCR_ALERT_ID_EMERGENCY ? "RELEASE" : "ACKNOWLEDGE");

    /* RADIO LOST stays up until the link recovers (spec 102): while the
     * vehicle is unreachable there is nothing to acknowledge and every other
     * page is dead - hiding the button keeps the state unmissable */
    if (s_cur_id == SCR_ALERT_ID_RADIO_LOST) {
        lv_obj_add_flag(s_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_btn, LV_OBJ_FLAG_HIDDEN);
    }

    if (!s_shown || s_cur_id != st->alert.id) {
        s_shown = true;
        lv_obj_remove_flag(s_ov, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_ov);
    }
}
