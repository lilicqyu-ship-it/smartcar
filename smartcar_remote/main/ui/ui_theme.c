#include "ui_theme.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

void ui_theme_apply_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_hex(UI_COL_TXT), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *ui_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_style_bg_color(c, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    /* HUD frame: 1 px outline instead of shadows - static, so it costs
     * nothing after the first paint (no per-frame blur on the RGB feed) */
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(UI_COL_LINE), 0);
    lv_obj_set_style_radius(c, 8, 0);
    lv_obj_set_style_pad_all(c, 8, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *ui_header(lv_obj_t *parent, const char *title, lv_event_cb_t on_back)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_set_size(bar, LV_PCT(100), 52);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_60, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *b = ui_button(bar, LV_SYMBOL_LEFT, lv_color_hex(UI_COL_SURFACE2), on_back, NULL);
    lv_obj_set_size(b, 56, 40);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 8, 0);

    lv_obj_t *t = lv_label_create(bar);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, F_LG, 0);
    lv_obj_center(t);

    return bar;
}

lv_obj_t *ui_kv_row(lv_obj_t *parent, const char *name, lv_obj_t **val_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 38);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, 10, 0);
    lv_obj_set_style_pad_ver(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *n = lv_label_create(row);
    lv_label_set_text(n, name);
    lv_obj_set_style_text_color(n, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_set_style_text_font(n, F_MD, 0);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *v = lv_label_create(row);
    lv_label_set_text(v, "-");
    lv_obj_set_style_text_font(v, F_MD, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);

    if (val_out) {
        *val_out = v;
    }
    return row;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t bg,
                    lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 8, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(UI_COL_LINE), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(UI_COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, F_MD, 0);
    lv_obj_center(l);

    if (cb) {
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user_data);
    }
    return b;
}

lv_color_t ui_col_for_state(bool ok, bool warn, bool crit)
{
    if (crit) return lv_color_hex(UI_COL_CRIT);
    if (warn) return lv_color_hex(UI_COL_WARN);
    if (ok)   return lv_color_hex(UI_COL_OK);
    return lv_color_hex(UI_COL_DIM);
}

void ui_label_set_text(lv_obj_t *lbl, const char *txt)
{
    if (strcmp(lv_label_get_text(lbl), txt) != 0) {
        lv_label_set_text(lbl, txt);
    }
}

void ui_label_set_fmt(lv_obj_t *lbl, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_label_set_text(lbl, buf);
}

void ui_label_set_color(lv_obj_t *obj, lv_color_t col)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(obj, 0), col)) {
        lv_obj_set_style_text_color(obj, col, 0);
    }
}
