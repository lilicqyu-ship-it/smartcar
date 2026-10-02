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
    lv_obj_t *root;         /* home page container (visibility gate) */
    lv_obj_t *conn;         /* top bar: connection state          */
    lv_obj_t *owner;        /* top bar: control owner             */
    lv_obj_t *speed;        /* big speed value                    */
    lv_obj_t *thr_bar;      /* throttle arc, left of the stick (-100..100) */
    lv_obj_t *thr_val;
    lv_obj_t *str_bar;      /* steering arc, under the stick              */
    lv_obj_t *str_val;
    lv_obj_t *spd_arc;      /* speed arc, over the stick                  */
    lv_obj_t *mode_btn[3];  /* ECO / NORMAL / SPORT segmented selector    */
    lv_obj_t *batt_bar;
    lv_obj_t *rssi_cap;     /* RSSI caption carries quality word  */
    lv_obj_t *batt;         /* info row 1 left                    */
    lv_obj_t *mode;         /* info row 1 middle                  */
    lv_obj_t *vstate;       /* info row 1 right                   */
    lv_obj_t *rssi;         /* info row 2 left                    */
    lv_obj_t *lat;          /* info row 2 middle                  */
    lv_obj_t *loss;         /* info row 2 right                   */
    lv_obj_t *cam;          /* top bar: CAM badge -> CAMERA page  */
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

static void cam_badge_cb(lv_event_t *e)
{
    (void)e;
    ui_nav_open(UI_PAGE_CAMERA);
}

/* one-touch mode select from the drive page (spec 33/34): each segment sets
 * its mode directly - no cycling through a mode you do not want */
static void mode_cb(lv_event_t *e)
{
    scr_settings_t set;
    scr_settings_get(&set);
    uint8_t m = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    if (m > SCR_MODE_SPORT) {
        m = SCR_MODE_NORMAL;
    }
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
static lv_obj_t *info_cell(lv_obj_t *rail, const char *cap, lv_obj_t **cap_out)
{
    /* one telemetry row of the right rail: small caption over a large value,
     * both fixed to a single line so nothing can wrap into its neighbour */
    lv_obj_t *cell = lv_obj_create(rail);
    lv_obj_set_size(cell, LV_PCT(100), 42);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cell, 0, 0);
    lv_obj_set_style_pad_all(cell, 0, 0);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *c = mk_label(cell, cap, F_SM, lv_color_hex(UI_COL_DIM));
    lv_obj_set_size(c, LV_PCT(100), lv_font_get_line_height(F_SM));
    lv_label_set_long_mode(c, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_letter_space(c, 2, 0);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, 0, 0);
    if (cap_out) {
        *cap_out = c;
    }

    lv_obj_t *v = mk_label(cell, "--", F_LG, lv_color_hex(UI_COL_TXT));
    lv_obj_set_size(v, LV_PCT(100), lv_font_get_line_height(F_LG));
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return v;
}

/* 30 Hz stick feedback: the THR/STR arcs follow the knob instead of the 10 Hz
 * page refresh (they used to lag the finger visibly).  Unchanged values do not
 * invalidate anything, so an idle stick costs nothing. */
static void stick_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_home.root == NULL || lv_obj_has_flag(s_home.root, LV_OBJ_FLAG_HIDDEN)) {
        return;                     /* home page not visible */
    }
    scr_state_t st;
    app_state_snapshot(&st);
    int thr = st.out_v * 100 / SCR_DRIVE_V_MAX;
    int str = st.out_w * 100 / SCR_DRIVE_W_MAX;
    if (lv_arc_get_value(s_home.thr_bar) != thr) {
        lv_arc_set_value(s_home.thr_bar, thr);
    }
    if (lv_arc_get_value(s_home.str_bar) != str) {
        lv_arc_set_value(s_home.str_bar, str);
    }
    ui_label_set_fmt(s_home.thr_val, "%+d%%", thr);
    ui_label_set_fmt(s_home.str_val, "%+d%%", str);
}

void ui_home_create(lv_obj_t *root)
{
    uint16_t w = bsp_display_get_h_res();
    uint16_t h = bsp_display_get_v_res();
    s_home.root = root;

    /* ---- layout: [ left control column | full-height square stick zone ] ------
     * The stick owns the right edge at the full panel height (480x480 on the
     * 800x480 panel) so the right thumb has the biggest possible pad; every
     * readout and control lives in the left column. */
    const int ZONE  = h;                /* square stick zone, full height   */
    const int LW    = w - ZONE;         /* left column width (320 on 800)   */
    const int BAR_H = 52;
    const int STOP_H = 60;

    lv_obj_t *lcol = lv_obj_create(root);
    lv_obj_set_size(lcol, LW, h);
    lv_obj_align(lcol, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(lcol, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(lcol, 0, 0);
    lv_obj_set_style_radius(lcol, 0, 0);
    lv_obj_set_style_pad_all(lcol, 0, 0);
    lv_obj_remove_flag(lcol, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- top bar (spec 10: left name, right connection + owner) ---------------- */
    lv_obj_t *bar = lv_obj_create(lcol);
    lv_obj_set_size(bar, LV_PCT(100), BAR_H);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 2, 0);
    lv_obj_set_style_border_color(bar, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_60, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);    /* default theme pad squeezed both sides together */
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Fixed horizontal budget (320 px column):
     *   [8..132 CAM entry][146..262 status][268..312 gear]
     * The "SMART CAR" title block that used to own x<146 is gone: the CAM entry
     * is this page's only way onto the video plane, and as one text row in the
     * status column it was an ~11 px (3 mm) target that taps never landed on.
     * Every label stays one line, fixed width, DOTS-truncated, so no string can
     * grow into its neighbour. */
    /* 150 px: the widest live readout, LV_SYMBOL_VIDEO " CAM 12.0 fps",
     * must fit whole - a DOTS-clipped "… 11.5" on the stream state is how
     * the drive page reads whether the video plane is alive */
    const int CAM_X = 8, CAM_W = 150, CAM_H = 40;
    const int GEAR_W = 44, GEAR_M = 8;
    const int STAT_W = LW - CAM_X - CAM_W - 14 - GEAR_W - GEAR_M - 6;

    /* CAM entry (S3Remote design doc 8.1): a real button that doubles as the
     * stream state readout - green fps / grey STALE / red OFFLINE, text +
     * colour never colour alone (spec 51).  Absent entirely when the video
     * plane is compiled out: a dead 124x40 button would be worse than none. */
#if CONFIG_SCR_CAM_WS_ENABLE
    lv_obj_t *cam_btn = lv_button_create(bar);
    lv_obj_set_size(cam_btn, CAM_W, CAM_H);
    lv_obj_align(cam_btn, LV_ALIGN_LEFT_MID, CAM_X, 0);
    lv_obj_set_style_bg_color(cam_btn, lv_color_hex(UI_COL_SURFACE2), 0);
    lv_obj_set_style_radius(cam_btn, 8, 0);
    lv_obj_set_style_shadow_width(cam_btn, 0, 0);
    lv_obj_set_style_border_width(cam_btn, 1, 0);
    lv_obj_set_style_border_color(cam_btn, lv_color_hex(UI_COL_LINE), 0);
    lv_obj_set_style_border_color(cam_btn, lv_color_hex(UI_COL_ACCENT), LV_STATE_PRESSED);
    s_home.cam = mk_label(cam_btn, "", F_SM, lv_color_hex(UI_COL_DIM));
    lv_obj_set_size(s_home.cam, CAM_W - 12, lv_font_get_line_height(F_SM));
    lv_label_set_long_mode(s_home.cam, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(s_home.cam);
    lv_obj_add_event_cb(cam_btn, cam_badge_cb, LV_EVENT_CLICKED, NULL);
#endif

    lv_obj_t *right = lv_obj_create(bar);
    lv_obj_set_size(right, STAT_W, BAR_H);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, -(GEAR_W + GEAR_M + 6), 0);
    s_home.conn = mk_label(right, "SEARCHING", F_SM, lv_color_hex(UI_COL_DIM));
    s_home.owner = mk_label(right, "NO CONTROL", F_SM, lv_color_hex(UI_COL_WARN));
    lv_obj_t *st_lbl[2] = { s_home.conn, s_home.owner };
    const int row_h = lv_font_get_line_height(F_SM);
    const int rows_top = (BAR_H - 2 * row_h) / 2;
    for (int i = 0; i < 2; i++) {
        lv_obj_set_size(st_lbl[i], STAT_W, row_h);
        lv_label_set_long_mode(st_lbl[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(st_lbl[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(st_lbl[i], LV_ALIGN_TOP_RIGHT, 0, rows_top + i * row_h);
    }

    lv_obj_t *gear = lv_button_create(bar);
    lv_obj_set_size(gear, GEAR_W, GEAR_W);
    lv_obj_align(gear, LV_ALIGN_RIGHT_MID, -GEAR_M, 0);
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

    /* ---- left column body: speed card on top, mode | telemetry below --------- */
    int body_h = h - BAR_H - STOP_H - 12;   /* inner height after 6 px padding */
    lv_obj_t *body = lv_obj_create(lcol);
    lv_obj_set_size(body, LW, body_h + 12);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, BAR_H);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_all(body, 6, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    const int SPD_H = 92;
    const int GAP   = 8;
    const int CW    = (LW - 12 - GAP) / 2;      /* column width (150 on 800) */
    const int COL_H = body_h - SPD_H - GAP;
    const int MB_H  = (COL_H - 22 - 2 * 6) / 3; /* mode button height        */

    lv_obj_t *scard = ui_card(body);
    lv_obj_set_size(scard, LV_PCT(100), SPD_H);
    lv_obj_align(scard, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(scard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sp_lbl = mk_label(scard, "SPEED", F_SM, lv_color_hex(UI_COL_ACCENT));
    lv_obj_set_style_text_letter_space(sp_lbl, 3, 0);
    lv_obj_align(sp_lbl, LV_ALIGN_TOP_LEFT, 2, 0);
    s_home.speed = mk_label(scard, "0.00", F_XXL, lv_color_hex(UI_COL_TXT));
    lv_obj_align(s_home.speed, LV_ALIGN_CENTER, 0, 4);
    lv_obj_t *unit = mk_label(scard, "m/s", F_MD, lv_color_hex(UI_COL_DIM));
    lv_obj_align(unit, LV_ALIGN_BOTTOM_RIGHT, -2, 0);

    /* mode selector column */
    lv_obj_t *left = lv_obj_create(body);
    lv_obj_set_size(left, CW, COL_H);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, 0, SPD_H + GAP);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mcap = mk_label(left, "DRIVE MODE", F_SM, lv_color_hex(UI_COL_ACCENT));
    lv_obj_set_style_text_letter_space(mcap, 2, 0);
    lv_obj_align(mcap, LV_ALIGN_TOP_LEFT, 4, 0);
    static const char *const mtxt[3] = { "ECO", "NORMAL", "SPORT" };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *mb = lv_button_create(left);
        lv_obj_set_size(mb, CW, MB_H);
        lv_obj_align(mb, LV_ALIGN_TOP_MID, 0, 22 + i * (MB_H + 6));
        lv_obj_set_style_radius(mb, 8, 0);
        lv_obj_set_style_shadow_width(mb, 0, 0);
        lv_obj_set_style_bg_color(mb, lv_color_hex(UI_COL_SURFACE), 0);
        lv_obj_set_style_border_width(mb, 1, 0);
        lv_obj_set_style_border_color(mb, lv_color_hex(UI_COL_LINE), 0);
        lv_obj_set_style_bg_color(mb, lv_color_hex(0x0B2A3A), LV_STATE_CHECKED);
        lv_obj_set_style_border_color(mb, lv_color_hex(UI_COL_ACCENT), LV_STATE_CHECKED);
        lv_obj_set_style_border_width(mb, 2, LV_STATE_CHECKED);
        lv_obj_add_event_cb(mb, mode_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *ml = mk_label(mb, mtxt[i], F_LG, lv_color_hex(UI_COL_DIM));
        lv_obj_set_style_text_letter_space(ml, 2, 0);
        lv_obj_set_style_text_color(ml, lv_color_hex(UI_COL_ACCENT), LV_STATE_CHECKED);
        lv_obj_add_flag(ml, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_center(ml);
        s_home.mode_btn[i] = mb;
    }
    s_home.mode = lv_obj_get_child(s_home.mode_btn[1], 0);  /* legacy handle */

    /* telemetry column, right of the mode selector */
    lv_obj_t *rail = ui_card(body);
    lv_obj_set_size(rail, CW, COL_H);
    lv_obj_align(rail, LV_ALIGN_TOP_RIGHT, 0, SPD_H + GAP);
    lv_obj_set_style_pad_all(rail, 8, 0);
    lv_obj_remove_flag(rail, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    s_home.batt   = info_cell(rail, "BATTERY", NULL);
    s_home.batt_bar = lv_bar_create(rail);
    lv_obj_set_size(s_home.batt_bar, LV_PCT(100), 6);
    lv_bar_set_range(s_home.batt_bar, 0, 100);
    lv_obj_set_style_radius(s_home.batt_bar, 2, 0);
    lv_obj_set_style_radius(s_home.batt_bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_home.batt_bar, lv_color_hex(UI_COL_SURFACE2), 0);
    lv_obj_set_style_bg_color(s_home.batt_bar, lv_color_hex(UI_COL_OK), LV_PART_INDICATOR);
    s_home.vstate = info_cell(rail, "STATE", NULL);
    s_home.rssi   = info_cell(rail, "RSSI", &s_home.rssi_cap);
    s_home.lat    = info_cell(rail, "PING", NULL);
    s_home.loss   = info_cell(rail, "LOSS", NULL);

    /* right: full-height square stick zone ringed by HUD arcs - throttle on
     * the left, steering below, speed above */
    lv_obj_t *zone = lv_obj_create(root);
    int zw = ZONE;
    lv_obj_set_size(zone, zw, ZONE);
    lv_obj_align(zone, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(zone, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(zone, 0, 0);
    lv_obj_set_style_radius(zone, 0, 0);
    lv_obj_set_style_pad_all(zone, 6, 0);
    lv_obj_remove_flag(zone, LV_OBJ_FLAG_SCROLLABLE);

    int ring = ZONE - 12 - 4;               /* arcs ring diameter */
    if (ring > zw - 8) {
        ring = zw - 8;
    }
    int joy_size = ring - 64;               /* pad inside the arcs */
    struct { lv_obj_t **o; int a0, a1; uint32_t col; bool sym; } arcs[3] = {
        { &s_home.thr_bar, 125, 235, UI_COL_ACCENT, true },   /* left: throttle  */
        { &s_home.str_bar,  55, 125, UI_COL_INFO,   true },   /* bottom: steer  */
        { &s_home.spd_arc, 235, 305, UI_COL_OK,     false },  /* top: speed     */
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *a = lv_arc_create(zone);
        lv_obj_set_size(a, ring, ring);
        lv_obj_center(a);
        lv_arc_set_bg_angles(a, arcs[i].a0, arcs[i].a1);
        lv_arc_set_range(a, arcs[i].sym ? -100 : 0, 100);
        lv_arc_set_mode(a, arcs[i].sym ? LV_ARC_MODE_SYMMETRICAL : LV_ARC_MODE_NORMAL);
        lv_arc_set_value(a, 0);
        lv_obj_remove_style(a, NULL, LV_PART_KNOB);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_arc_width(a, 8, 0);
        lv_obj_set_style_arc_color(a, lv_color_hex(UI_COL_SURFACE2), 0);
        lv_obj_set_style_arc_rounded(a, false, 0);
        lv_obj_set_style_arc_width(a, 8, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(a, lv_color_hex(arcs[i].col), LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(a, false, LV_PART_INDICATOR);
        *arcs[i].o = a;
    }
    /* arc readouts in the free corners of the zone */
    lv_obj_t *tl = mk_label(zone, "THR", F_SM, lv_color_hex(UI_COL_ACCENT));
    lv_obj_set_style_text_letter_space(tl, 2, 0);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 0, 0);
    s_home.thr_val = mk_label(zone, "+0%", F_LG, lv_color_hex(UI_COL_TXT));
    lv_obj_align(s_home.thr_val, LV_ALIGN_TOP_LEFT, 0, 18);
    lv_obj_t *sl = mk_label(zone, "STR", F_SM, lv_color_hex(UI_COL_INFO));
    lv_obj_set_style_text_letter_space(sl, 2, 0);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_LEFT, 0, -24);
    s_home.str_val = mk_label(zone, "+0%", F_LG, lv_color_hex(UI_COL_TXT));
    lv_obj_align(s_home.str_val, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *fw = mk_label(zone, LV_SYMBOL_UP " FWD", F_SM, lv_color_hex(UI_COL_DIM));
    lv_obj_align(fw, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_t *rv = mk_label(zone, LV_SYMBOL_DOWN " REV", F_SM, lv_color_hex(UI_COL_DIM));
    lv_obj_align(rv, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_obj_t *joy = ui_joystick_create(zone, joy_size);
    lv_obj_center(joy);
    lv_timer_create(stick_timer_cb, 33, NULL);

    /* ---- STOP (spec 18: big, fixed, bottom of the control column) --------------- */
    lv_obj_t *stop_area = lv_obj_create(lcol);
    lv_obj_set_size(stop_area, LW, STOP_H);
    lv_obj_align(stop_area, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(stop_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stop_area, 0, 0);
    lv_obj_set_style_pad_all(stop_area, 0, 0);
    lv_obj_remove_flag(stop_area, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *stop = lv_button_create(stop_area);
    lv_obj_set_size(stop, LW - 12, 52);
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

    /* throttle / steering arcs: driven by stick_timer_cb at 30 Hz */

    int spd = 0;
    if (st->tele_fresh) {
        int32_t a = st->speed_mm_s < 0 ? -st->speed_mm_s : st->speed_mm_s;
        spd = (int)(a * 100 / (SCR_DRIVE_V_MAX > 0 ? SCR_DRIVE_V_MAX : 1));
        spd = spd > 100 ? 100 : spd;
    }
    if (lv_arc_get_value(s_home.spd_arc) != spd) {
        lv_arc_set_value(s_home.spd_arc, spd);
    }

    /* battery / mode / vehicle state */
    if (st->tele_fresh && st->batt_pct > 0) {
        ui_label_set_fmt(s_home.batt, "%u%%", st->batt_pct);
        if (lv_bar_get_value(s_home.batt_bar) != st->batt_pct) {
            lv_bar_set_value(s_home.batt_bar, st->batt_pct, LV_ANIM_OFF);
        }
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
    (void)mode_txt;
    for (int i = 0; i < 3; i++) {
        bool on = (i == m);
        if (on != lv_obj_has_state(s_home.mode_btn[i], LV_STATE_CHECKED)) {
            if (on) {
                lv_obj_add_state(s_home.mode_btn[i], LV_STATE_CHECKED);
                lv_obj_add_state(lv_obj_get_child(s_home.mode_btn[i], 0), LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(s_home.mode_btn[i], LV_STATE_CHECKED);
                lv_obj_remove_state(lv_obj_get_child(s_home.mode_btn[i], 0), LV_STATE_CHECKED);
            }
        }
    }

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

#if CONFIG_SCR_CAM_WS_ENABLE
    /* CAM entry: the glyph is what makes it read as a button, the text +
     * colour carry the stream state (design doc 8.1, spec 51) */
    if (st->cam.conn == SCR_CAM_CONNECTED) {
        if (st->cam.stale) {
            ui_label_set_text(s_home.cam, LV_SYMBOL_VIDEO " CAM STALE");
            ui_label_set_color(s_home.cam, lv_color_hex(UI_COL_DIM));
        } else if (st->cam.fps_x10 > 0) {
            ui_label_set_fmt(s_home.cam, LV_SYMBOL_VIDEO " CAM %u.%u fps",
                             st->cam.fps_x10 / 10, st->cam.fps_x10 % 10);
            ui_label_set_color(s_home.cam, lv_color_hex(UI_COL_OK));
        } else {
            ui_label_set_text(s_home.cam, LV_SYMBOL_VIDEO " CAM IDLE");
            ui_label_set_color(s_home.cam, lv_color_hex(UI_COL_DIM));
        }
    } else if (st->cam.conn == SCR_CAM_CONNECTING) {
        ui_label_set_text(s_home.cam, LV_SYMBOL_VIDEO " CAM LINK");
        ui_label_set_color(s_home.cam, lv_color_hex(UI_COL_INFO));
    } else {
        ui_label_set_text(s_home.cam, LV_SYMBOL_VIDEO " CAM OFFLINE");
        ui_label_set_color(s_home.cam, lv_color_hex(UI_COL_CRIT));
    }
#endif
}
