#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ui.h"
#include "ui_theme.h"
#include "ui_home.h"
#include "ui_pages.h"
#include "ui_alert.h"

#include "../app_state.h"
#include "../scr_link.h"
#include "../scr_ctrl.h"
#include "../scr_settings.h"
#include "../proto/proto_frames.h"

#define UI_PERIOD_MS    100     /* 10 Hz state -> UI refresh (spec 75)  */

typedef void (*ui_page_create_fn)(lv_obj_t *root);
typedef void (*ui_page_refresh_fn)(const scr_state_t *st);

static lv_obj_t *s_scr_main;
static lv_obj_t *s_scr_boot;
static lv_obj_t *s_pages[UI_PAGE_COUNT];
static ui_page_t s_cur = UI_PAGE_HOME;
static bool s_engineer;

/* boot screen widgets */
static lv_obj_t *s_boot_bar;
static lv_obj_t *s_boot_chk[4];
static int64_t   s_boot_all_ms;
static bool      s_boot_switching;

/* toast */
static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;

static const char * const BOOT_NAMES[4] = { "LCD", "Touch", "Radio", "System" };

bool ui_engineer_mode(void)
{
    return s_engineer;
}

void ui_engineer_enable(void)
{
    if (!s_engineer) {
        s_engineer = true;
        ui_toast("ENGINEER MODE ON");
        app_state_log(SCR_LOG_INFO, "Engineer mode on");
        ui_pages_rebuild();
    }
}

/* ---- toast ----------------------------------------------------------------*/
static void toast_timer_cb(lv_timer_t *t)
{
    if (s_toast) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    }
    lv_timer_del(t);
    s_toast_timer = NULL;
}

void ui_toast(const char *fmt, ...)
{
    if (s_toast == NULL) {
        return;
    }
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    lv_label_set_text(s_toast, buf);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_toast);

    if (s_toast_timer) {
        lv_timer_del(s_toast_timer);
    }
    s_toast_timer = lv_timer_create(toast_timer_cb, 1400, NULL);
}

/* ---- navigation --------------------------------------------------------------*/
void ui_nav_open(ui_page_t p)
{
    if (p >= UI_PAGE_COUNT || p == s_cur) {
        return;
    }
    lv_obj_add_flag(s_pages[s_cur], LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_pages[p], LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_pages[p]);
    lv_obj_move_foreground(s_toast);
    s_cur = p;

    /* stale-data guard: a page never shows values captured before it opened
     * (spec 106) - every visible page is re-painted from the next tick on */
}

/* ---- boot screen (P0, spec 6/7: no debug spam, just stage check marks) --------*/
static void boot_create(lv_obj_t *scr)
{
    ui_theme_apply_screen(scr);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "SMART CAR");
    lv_obj_set_style_text_font(title, F_XL, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "REMOTE CONTROLLER");
    ui_label_set_color(sub, lv_color_hex(UI_COL_DIM));
    lv_obj_set_style_text_font(sub, F_MD, 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 100);

    lv_obj_t *car = lv_label_create(scr);
    lv_label_set_text(car, LV_SYMBOL_CHARGE);   /* placeholder glyph, phase 6 art */
    lv_obj_set_style_text_font(car, F_XXL, 0);
    ui_label_set_color(car, lv_color_hex(UI_COL_ACCENT));
    lv_obj_align(car, LV_ALIGN_TOP_MID, 0, 150);

    s_boot_bar = lv_bar_create(scr);
    lv_obj_set_size(s_boot_bar, 260, 10);
    lv_bar_set_range(s_boot_bar, 0, 4);
    lv_bar_set_value(s_boot_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_boot_bar, lv_color_hex(UI_COL_SURFACE2), 0);
    lv_obj_set_style_bg_opa(s_boot_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_boot_bar, lv_color_hex(UI_COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_align(s_boot_bar, LV_ALIGN_TOP_MID, 0, 250);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *row = lv_obj_create(scr);
        lv_obj_set_size(row, 200, 30);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 285 + i * 34);

        lv_obj_t *n = lv_label_create(row);
        lv_label_set_text(n, BOOT_NAMES[i]);
        lv_obj_set_style_text_font(n, F_MD, 0);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *c = lv_label_create(row);
        lv_label_set_text(c, "-");
        ui_label_set_color(c, lv_color_hex(UI_COL_DIM));
        lv_obj_set_style_text_font(c, F_MD, 0);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, 0, 0);
        s_boot_chk[i] = c;
    }

    lv_obj_t *foot = lv_label_create(scr);
    lv_label_set_text(foot, "SYSTEM STARTING");
    ui_label_set_color(foot, lv_color_hex(UI_COL_DIM));
    lv_obj_set_style_text_font(foot, F_SM, 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -40);
}

static void boot_refresh(const scr_state_t *st)
{
    bool flags[4] = { st->boot_lcd, st->boot_touch, st->boot_radio, st->boot_sys };
    int done = 0;
    for (int i = 0; i < 4; i++) {
        lv_obj_t *c = s_boot_chk[i];
        ui_label_set_color(c, flags[i] ? lv_color_hex(UI_COL_OK)
                                       : lv_color_hex(UI_COL_DIM));
        ui_label_set_text(c, flags[i] ? LV_SYMBOL_OK : "-");
        if (flags[i]) {
            done++;
        }
    }
    lv_bar_set_value(s_boot_bar, done, LV_ANIM_OFF);

    if (done == 4) {
        if (s_boot_all_ms == 0) {
            s_boot_all_ms = st->uptime_ms;
        } else if (!s_boot_switching && st->uptime_ms - s_boot_all_ms > 600) {
            s_boot_switching = true;
            /* P0 done -> P1; instant switch: a fade would blend two full
             * 800x480 screens for 250 ms and strobe the panel (spec 49:
             * transitions must not cost stability) */
            lv_screen_load_anim(s_scr_main, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
        }
    }
}

/* ---- periodic refresh ------------------------------------------------------------*/
static void ui_timer_cb(lv_timer_t *t)
{
    scr_state_t st;
    app_state_snapshot(&st);

    if (!s_boot_switching) {
        boot_refresh(&st);
        return;
    }

    ui_alert_refresh(&st);          /* system-level overlay, transition-gated */

    switch (s_cur) {
        case UI_PAGE_HOME:     ui_home_refresh(&st);     break;
        case UI_PAGE_VEHICLE:  ui_pages_vehicle_refresh(&st);  break;
        case UI_PAGE_RADIO:    ui_pages_radio_refresh(&st);    break;
        case UI_PAGE_DIAG:     ui_pages_diag_refresh(&st);     break;
        case UI_PAGE_SETTINGS: ui_pages_settings_refresh(&st); break;
        case UI_PAGE_PAIR:     ui_pages_pair_refresh(&st);     break;
        case UI_PAGE_EVENTS:   ui_pages_events_refresh(&st);   break;
        default: break;
    }
}

/* ---- init -------------------------------------------------------------------------*/
void ui_init(void)
{
    /* boot screen */
    s_scr_boot = lv_obj_create(NULL);
    boot_create(s_scr_boot);
    lv_screen_load(s_scr_boot);

    /* main screen with all pages stacked (hidden except the active one) */
    s_scr_main = lv_obj_create(NULL);
    ui_theme_apply_screen(s_scr_main);

    s_pages[UI_PAGE_HOME]     = lv_obj_create(s_scr_main);
    s_pages[UI_PAGE_VEHICLE]  = lv_obj_create(s_scr_main);
    s_pages[UI_PAGE_RADIO]    = lv_obj_create(s_scr_main);
    s_pages[UI_PAGE_DIAG]     = lv_obj_create(s_scr_main);
    s_pages[UI_PAGE_SETTINGS] = lv_obj_create(s_scr_main);
    s_pages[UI_PAGE_PAIR]     = lv_obj_create(s_scr_main);
    s_pages[UI_PAGE_EVENTS]   = lv_obj_create(s_scr_main);

    for (int i = 0; i < UI_PAGE_COUNT; i++) {
        lv_obj_t *p = s_pages[i];
        lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(p, lv_color_hex(UI_COL_BG), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(p, 0, 0);
        lv_obj_set_style_pad_all(p, 0, 0);
        lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    }

    ui_home_create(s_pages[UI_PAGE_HOME]);
    ui_pages_create(s_pages[UI_PAGE_VEHICLE], s_pages[UI_PAGE_RADIO],
                    s_pages[UI_PAGE_DIAG], s_pages[UI_PAGE_SETTINGS],
                    s_pages[UI_PAGE_PAIR], s_pages[UI_PAGE_EVENTS]);
    ui_alert_create(s_scr_main);

    /* toast on the top layer so every page can show feedback */
    s_toast = lv_label_create(lv_layer_top());
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(UI_COL_SURFACE2), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_toast, 10, 0);
    lv_obj_set_style_pad_hor(s_toast, 14, 0);
    lv_obj_set_style_pad_ver(s_toast, 8, 0);
    lv_obj_set_style_text_font(s_toast, F_MD, 0);
    lv_label_set_text(s_toast, "");
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    lv_obj_remove_flag(s_pages[UI_PAGE_HOME], LV_OBJ_FLAG_HIDDEN);
    s_cur = UI_PAGE_HOME;

    lv_timer_create(ui_timer_cb, UI_PERIOD_MS, NULL);
}
