#include <stdio.h>
#include <esp_timer.h>
#include <sdkconfig.h>
#include <bsp/esp-bsp.h>

#include "ui_home.h"
#include "ui_theme.h"
#include "ui_joystick.h"
#include "ui.h"

#include "../scr_ctrl.h"
#include "../scr_settings.h"
#include "../app_state.h"
#include "../proto/proto_frames.h"

typedef struct {
    lv_obj_t *conn;         /* top bar: connection state          */
    lv_obj_t *owner;        /* top bar: control owner             */
    lv_obj_t *speed;        /* big speed value                    */
    lv_obj_t *thr_bar;      /* throttle bar (symmetric -100..100) */
    lv_obj_t *thr_val;
    lv_obj_t *str_bar;      /* steering bar                       */
    lv_obj_t *str_val;
    lv_obj_t *rssi_cap;     /* RSSI caption carries quality word  */
    lv_obj_t *batt;         /* info row 1 left                    */
    lv_obj_t *mode;         /* info row 1 middle                  */
    lv_obj_t *vstate;       /* info row 1 right                   */
    lv_obj_t *rssi;         /* info row 2 left                    */
    lv_obj_t *lat;          /* info row 2 middle                  */
    lv_obj_t *loss;         /* info row 2 right                   */
    lv_obj_t *stop_lbl;
    int64_t   press_ms;
} home_t;

static home_t s_home;

/* ---- STOP: tap = stop, hold = emergency (spec 19) -------------------------------*/
static void stop_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_home.press_ms = esp_timer_get_time() / 1000;
    } else if (code == LV_EVENT_RELEASED) {
        /* guard against a RELEASED without a tracked PRESSED (e.g. press lost
         * behind the alert overlay) - that must never read as a long hold */
        if (s_home.press_ms == 0) {
            scr_ctrl_stop_button();
            return;
        }
        int64_t held = esp_timer_get_time() / 1000 - s_home.press_ms;
        s_home.press_ms = 0;
        if (held >= CONFIG_SCR_EMERG_LONGPRESS_MS) {
            scr_ctrl_emergency();
        } else {
            scr_ctrl_stop_button();
        }
    }
}

static void gear_cb(lv_event_t *e)
{
    ui_nav_open(UI_PAGE_SETTINGS);
}

/* one-touch mode cycling from the drive page (spec 33/34) */
static void mode_cb(lv_event_t *e)
{
    scr_settings_t set;
    scr_settings_get(&set);
    uint8_t m = (set.mode + 1) % (SCR_MODE_SPORT + 1);
    scr_settings_set_control(m, set.deadzone_pct);
    app_state_set_mode((scr_mode_t)m);
    ui_toast("%s MODE", m == SCR_MODE_ECO ? "ECO" : m == SCR_MODE_NORMAL ? "NORMAL" : "SPORT");
}

/* mm/s -> "+1.25" (m/s, 2 decimals) without pulling float printf into LVGL.
 * Sign comes from the full value: int division truncates toward zero, so
 * -50 mm/s is "-0.05", never "+0.05". */
static const char *fmt_speed(int32_t mm_s)
{
    static char buf[12];
    int32_t v = mm_s < 0 ? -mm_s : mm_s;
    snprintf(buf, sizeof(buf), "%c%d.%02d",
             mm_s < 0 ? '-' : '+', (int)(v / 1000), (int)((v % 1000) / 10));
    return buf;
}

static const char *sys_state_text(scr_sys_t sys)
{
    switch (sys) {
        case SCR_SYS_BOOT:       return "BOOT";
        case SCR_SYS_CONNECTING: return "LINKING";
        case SCR_SYS_READY:      return "READY";
        case SCR_SYS_CONTROL:    return "DRIVING";
        case SCR_SYS_WARNING:    return "WARNING";
        case SCR_SYS_FAULT:      return "FAULT";
        case SCR_SYS_STOPPED:    return "STOPPED";
        case SCR_SYS_EMERGENCY:  return "E-STOP";
        default:                 return "-";
    }
}

static lv_color_t sys_state_color(scr_sys_t sys)
{
    switch (sys) {
        case SCR_SYS_READY:
        case SCR_SYS_CONTROL:    return lv_color_hex(UI_COL_OK);
        case SCR_SYS_WARNING:
        case SCR_SYS_CONNECTING: return lv_color_hex(UI_COL_WARN);
        case SCR_SYS_FAULT:
        case SCR_SYS_EMERGENCY:  return lv_color_hex(UI_COL_CRIT);
        default:                 return lv_color_hex(UI_COL_DIM);
    }
}

/* dim secondary label */
static lv_obj_t *mk_label(lv_obj_t *parent, const char *txt, const lv_font_t *f,
                          lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    ui_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    ui_label_set_color(l, col);
    return l;
}

/* One fixed-width telemetry cell: dim caption over a value.  Each cell owns
 * exactly a third of the card and its labels clip with "..." inside it, so a
 * long value ("-68 dBm EXCELLENT", "LOSS 10.0%") can never spill into its
 * neighbour - the old space-between row let three free-width labels overlap. */
static lv_obj_t *info_cell(lv_obj_t *grid, const char *cap, lv_obj_t **cap_out)
{
    lv_obj_t *cell = lv_obj_create(grid);
    lv_obj_set_size(cell, LV_PCT(33), 40);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *c = mk_label(cell, cap, F_SM, lv_color_hex(UI_COL_DIM));
    /* fixed one-line height: DOTS would otherwise wrap and overlap the value */
    lv_obj_set_size(c, LV_PCT(100), lv_font_get_line_height(F_SM));
    lv_label_set_long_mode(c, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 0);
    if (cap_out) {
        *cap_out = c;
    }

    lv_obj_t *v = mk_label(cell, "--", F_MD, lv_color_hex(UI_COL_TXT));
    lv_obj_set_size(v, LV_PCT(100), lv_font_get_line_height(F_MD));
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(v, LV_ALIGN_BOTTOM_MID, 0, 0);
    return v;
}

/* HUD bar row: caption | symmetric bar | value.  Fixed widths, no overlap. */
static lv_obj_t *bar_row(lv_obj_t *parent, const char *cap, lv_obj_t **val_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), 22);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *c = mk_label(row, cap, F_SM, lv_color_hex(UI_COL_DIM));
    lv_obj_set_width(c, 44);
    lv_obj_align(c, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *b = lv_bar_create(row);
    lv_obj_set_height(b, 8);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_style_margin_left(b, 0, 0);
    lv_bar_set_range(b, -100, 100);
    lv_bar_set_mode(b, LV_BAR_MODE_SYMMETRICAL);
    lv_bar_set_value(b, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_radius(b, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_SURFACE2), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(b, 0, 0);

    lv_obj_t *v = mk_label(row, "+0%", F_SM, lv_color_hex(UI_COL_TXT));
    lv_obj_set_width(v, 48);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);

    /* bar fills what is left between caption and value */
    lv_obj_update_layout(row);
    lv_obj_set_width(b, lv_obj_get_content_width(row) - 44 - 48 - 8);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 44, 0);

    *val_out = v;
    return b;
}

void ui_home_create(lv_obj_t *root)
{
    uint16_t w = bsp_display_get_h_res();
    uint16_t h = bsp_display_get_v_res();
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* ---- top bar (spec 10: left name, right connection + owner) ---------------- */
    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_set_size(bar, LV_PCT(100), 52);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_60, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* accent tick + spaced title: HUD header */
    lv_obj_t *tick = lv_obj_create(bar);
    lv_obj_set_size(tick, 4, 24);
    lv_obj_set_style_radius(tick, 1, 0);
    lv_obj_set_style_border_width(tick, 0, 0);
    lv_obj_set_style_bg_color(tick, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, 0);
    lv_obj_align(tick, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *title = mk_label(bar, "SMART CAR", F_LG, lv_color_hex(UI_COL_TXT));
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 24, -7);
    lv_obj_t *sub = mk_label(bar, "REMOTE CONSOLE", F_SM, lv_color_hex(UI_COL_ACCENT));
    lv_obj_set_style_text_letter_space(sub, 2, 0);
    lv_obj_align(sub, LV_ALIGN_LEFT_MID, 24, 13);

    lv_obj_t *right = lv_obj_create(bar);
    /* explicit height: LV_SIZE_CONTENT only bounds one child, so the stacked
     * connection/owner labels would print on top of each other */
    lv_obj_set_size(right, LV_SIZE_CONTENT, 38);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -62, 0);
    s_home.conn = mk_label(right, "SEARCHING", F_SM, lv_color_hex(UI_COL_DIM));
    s_home.owner = mk_label(right, "NO CONTROL", F_SM, lv_color_hex(UI_COL_WARN));
    lv_obj_align(s_home.conn, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_align(s_home.owner, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_obj_t *gear = lv_button_create(bar);
    lv_obj_set_size(gear, 44, 44);
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(gear, lv_color_hex(UI_COL_SURFACE2), 0);
    lv_obj_set_style_radius(gear, 8, 0);
    lv_obj_set_style_shadow_width(gear, 0, 0);
    lv_obj_set_style_border_width(gear, 1, 0);
    lv_obj_set_style_border_color(gear, lv_color_hex(UI_COL_LINE), 0);
    lv_obj_set_style_border_color(gear, lv_color_hex(UI_COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_t *gear_lbl = lv_label_create(gear);
    ui_label_set_text(gear_lbl, LV_SYMBOL_SETTINGS);
    lv_obj_center(gear_lbl);
    lv_obj_add_event_cb(gear, gear_cb, LV_EVENT_CLICKED, NULL);

    /* ---- body: left telemetry column + joystick zone --------------------------- */
    lv_obj_t *body = lv_obj_create(root);
    lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 6, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);

    lv_obj_t *left = lv_obj_create(body);
    lv_obj_set_size(left, (int)(w * 0.36f), LV_PCT(100));
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);

    lv_obj_set_style_pad_row(left, 6, 0);

    /* speed card (spec 17: big number + unit) */
    lv_obj_t *scard = ui_card(left);
    lv_obj_set_size(scard, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(scard, 1);
    lv_obj_t *sp_lbl = mk_label(scard, "SPEED", F_SM, lv_color_hex(UI_COL_ACCENT));
    lv_obj_set_style_text_letter_space(sp_lbl, 3, 0);
    lv_obj_align(sp_lbl, LV_ALIGN_TOP_LEFT, 2, 0);
    s_home.speed = mk_label(scard, "0.00", F_XXL, lv_color_hex(UI_COL_TXT));
    lv_obj_align(s_home.speed, LV_ALIGN_CENTER, 0, 0);
    lv_obj_t *unit = mk_label(scard, "m/s", F_MD, lv_color_hex(UI_COL_DIM));
    lv_obj_align(unit, LV_ALIGN_BOTTOM_RIGHT, -2, 0);

    /* throttle / steering of what is actually sent (spec 16) as HUD bars */
    lv_obj_t *dcard = ui_card(left);
    lv_obj_set_size(dcard, LV_PCT(100), 62);
    lv_obj_set_style_pad_ver(dcard, 6, 0);
    lv_obj_set_flex_flow(dcard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dcard, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_home.thr_bar = bar_row(dcard, "THR", &s_home.thr_val);
    s_home.str_bar = bar_row(dcard, "STR", &s_home.str_val);

    /* telemetry grid (spec 70): 3 x 2 fixed cells */
    lv_obj_t *icard = ui_card(left);
    lv_obj_set_size(icard, LV_PCT(100), 100);
    lv_obj_set_style_pad_all(icard, 6, 0);
    lv_obj_set_style_pad_row(icard, 4, 0);
    lv_obj_set_style_pad_column(icard, 0, 0);
    lv_obj_set_flex_flow(icard, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(icard, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    s_home.batt   = info_cell(icard, "BATTERY", NULL);
    s_home.mode   = info_cell(icard, "MODE", NULL);
    s_home.vstate = info_cell(icard, "STATE", NULL);
    s_home.rssi   = info_cell(icard, "RSSI", &s_home.rssi_cap);
    s_home.lat    = info_cell(icard, "PING", NULL);
    s_home.loss   = info_cell(icard, "LOSS", NULL);
    /* mode cycling: the whole cell is the touch target, not just the text */
    lv_obj_add_flag(lv_obj_get_parent(s_home.mode), LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lv_obj_get_parent(s_home.mode), mode_cb, LV_EVENT_CLICKED, NULL);

    /* joystick zone (spec 13-16) */
    lv_obj_t *zone = lv_obj_create(body);
    lv_obj_set_flex_grow(zone, 1);
    lv_obj_set_size(zone, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(zone, 0, 0);
    lv_obj_set_style_pad_all(zone, 0, 0);
    lv_obj_remove_flag(zone, LV_OBJ_FLAG_SCROLLABLE);

    int body_h = h - 52 - 56 - 12;
    int joy_size = body_h - 8;
    if (joy_size > 340) {
        joy_size = 340;
    }
    lv_obj_t *joy = ui_joystick_create(zone, joy_size);
    lv_obj_center(joy);

    /* ---- STOP (spec 18: big, fixed, bottom centre) ------------------------------ */
    lv_obj_t *stop_area = lv_obj_create(root);
    lv_obj_set_size(stop_area, LV_PCT(100), 56);
    lv_obj_set_style_bg_opa(stop_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stop_area, 0, 0);
    lv_obj_set_style_pad_all(stop_area, 0, 0);
    lv_obj_remove_flag(stop_area, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *stop = lv_button_create(stop_area);
    int sw = (w * 5) / 12;
    lv_obj_set_size(stop, sw, 50);
    lv_obj_center(stop);
    lv_obj_set_style_bg_color(stop, lv_color_hex(UI_COL_CRIT), 0);
    lv_obj_set_style_bg_color(stop, lv_color_hex(0xC22F3F), LV_STATE_PRESSED);
    lv_obj_set_style_radius(stop, 8, 0);
    lv_obj_set_style_shadow_width(stop, 0, 0);
    lv_obj_set_style_border_width(stop, 2, 0);
    lv_obj_set_style_border_color(stop, lv_color_hex(0xFF9AA8), 0);
    lv_obj_add_event_cb(stop, stop_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(stop, stop_cb, LV_EVENT_RELEASED, NULL);

    s_home.stop_lbl = mk_label(stop, "STOP", F_XL, lv_color_hex(0xFFFFFF));
    lv_obj_set_style_text_letter_space(s_home.stop_lbl, 6, 0);
    lv_obj_center(s_home.stop_lbl);
}

void ui_home_refresh(const scr_state_t *st)
{
    /* connection: icon + text, never colour alone (spec 11/51) */
    const char *conn_txt;
    lv_color_t conn_col;
    if (st->conn == SCR_CONN_CONNECTED) {
        if (st->tele_fresh) { conn_txt = "CONNECTED"; conn_col = lv_color_hex(UI_COL_OK); }
        else                { conn_txt = "STALE";     conn_col = lv_color_hex(UI_COL_WARN); }
    } else if (st->conn == SCR_CONN_CONNECTING) {
        conn_txt = "CONNECTING"; conn_col = lv_color_hex(UI_COL_INFO);
    } else {
        conn_txt = "SEARCHING"; conn_col = lv_color_hex(UI_COL_DIM);
    }
    ui_label_set_text(s_home.conn, conn_txt);
    ui_label_set_color(s_home.conn, conn_col);

    /* control owner (spec 12/103) */
    const char *owner_txt;
    lv_color_t owner_col;
    switch (st->owner) {
        case SCR_OWNER_S3:  owner_txt = "S3 MASTER";  owner_col = lv_color_hex(UI_COL_OK);   break;
        case SCR_OWNER_WEB: owner_txt = "WEB MASTER"; owner_col = lv_color_hex(UI_COL_INFO); break;
        default:            owner_txt = "NO CONTROL"; owner_col = lv_color_hex(UI_COL_WARN); break;
    }
    ui_label_set_text(s_home.owner, owner_txt);
    ui_label_set_color(s_home.owner, owner_col);

    /* speed (spec 101/102: stale data shows as --) */
    if (st->tele_fresh) {
        ui_label_set_text(s_home.speed, fmt_speed(st->speed_mm_s));
        ui_label_set_color(s_home.speed, lv_color_hex(UI_COL_TXT));
    } else {
        ui_label_set_text(s_home.speed, "--");
        ui_label_set_color(s_home.speed, lv_color_hex(UI_COL_DIM));
    }

    /* throttle / steering of what was actually sent (spec 16: 油门非零可见);
     * the bar/label setters are no-ops for unchanged values, so an idle stick
     * invalidates nothing */
    int thr = st->out_v * 100 / SCR_DRIVE_V_MAX;
    int str = st->out_w * 100 / SCR_DRIVE_W_MAX;
    if (lv_bar_get_value(s_home.thr_bar) != thr) {
        lv_bar_set_value(s_home.thr_bar, thr, LV_ANIM_OFF);
    }
    if (lv_bar_get_value(s_home.str_bar) != str) {
        lv_bar_set_value(s_home.str_bar, str, LV_ANIM_OFF);
    }
    ui_label_set_fmt(s_home.thr_val, "%+d%%", thr);
    ui_label_set_fmt(s_home.str_val, "%+d%%", str);

    /* battery / mode / vehicle state */
    if (st->tele_fresh && st->batt_pct > 0) {
        ui_label_set_fmt(s_home.batt, "%u%%", st->batt_pct);
        lv_obj_set_style_text_color(s_home.batt,
            ui_col_for_state(st->batt_pct > CONFIG_SCR_BATT_LOW_PCT,
                             st->batt_pct <= CONFIG_SCR_BATT_LOW_PCT &&
                             st->batt_pct > CONFIG_SCR_BATT_CRIT_PCT,
                             st->batt_pct <= CONFIG_SCR_BATT_CRIT_PCT), 0);
    } else {
        ui_label_set_text(s_home.batt, "--");
        ui_label_set_color(s_home.batt, lv_color_hex(UI_COL_DIM));
    }

    static const char * const mode_txt[3] = { "ECO", "NORMAL", "SPORT" };
    scr_settings_t set;
    scr_settings_get(&set);
    uint8_t m = set.mode <= SCR_MODE_SPORT ? set.mode : 1;
    ui_label_set_color(s_home.mode, lv_color_hex(UI_COL_ACCENT));
    ui_label_set_text(s_home.mode, mode_txt[m]);   /* tap cell to cycle (spec 33/34) */

    /* joystick live-input gate: only while the S3 actually holds control */
    ui_joystick_set_enabled(st->conn == SCR_CONN_CONNECTED &&
                            st->owner == SCR_OWNER_S3);

    scr_sys_t sys = app_state_derive(st);
    ui_label_set_text(s_home.vstate, sys_state_text(sys));
    ui_label_set_color(s_home.vstate, sys_state_color(sys));

    /* radio cell: value = dBm, caption = quality word (spec 24/51: text,
     * not colour alone) */
    if (st->rssi != 0) {
        static const char * const qual_txt[6] = { "RSSI", "RSSI EXC", "RSSI GOOD",
                                                  "RSSI FAIR", "RSSI WEAK", "RSSI CRIT" };
        lv_color_t qc = ui_col_for_state(st->quality >= SCR_QUAL_GOOD,
                                         st->quality == SCR_QUAL_FAIR || st->quality == SCR_QUAL_WEAK,
                                         st->quality == SCR_QUAL_CRITICAL);
        ui_label_set_fmt(s_home.rssi, "%d dBm", st->rssi);
        ui_label_set_color(s_home.rssi, qc);
        ui_label_set_text(s_home.rssi_cap, qual_txt[st->quality <= 5 ? st->quality : 0]);
        ui_label_set_color(s_home.rssi_cap, qc);
    } else {
        ui_label_set_text(s_home.rssi, "--");
        ui_label_set_color(s_home.rssi, lv_color_hex(UI_COL_DIM));
        ui_label_set_text(s_home.rssi_cap, "RSSI");
        ui_label_set_color(s_home.rssi_cap, lv_color_hex(UI_COL_DIM));
    }

    /* latency/loss are measured 1 Hz on a live link; before that they are
     * meaningless zeros, show the stale marker instead (spec 101) */
    if (st->conn == SCR_CONN_CONNECTED) {
        ui_label_set_fmt(s_home.lat, "%u ms", st->lat_ms);
        ui_label_set_fmt(s_home.loss, "%u.%u%%",
                              st->loss_pct_x10 / 10, st->loss_pct_x10 % 10);
    } else {
        ui_label_set_text(s_home.lat, "--");
        ui_label_set_text(s_home.loss, "--");
    }
}
