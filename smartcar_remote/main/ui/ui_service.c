/*
 * ui_service.c - FIRMWARE / CALIBRATE / DIAGNOSE pages (doc/08 §4-§7).
 *
 * Rendering rules shared with the rest of the HUD: static outlines only, cached
 * label/colour setters, nothing repaints unless its value changed.  Pages never
 * block: every request goes to scr_svc (core 0) or the ctrl task.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#include "esp_timer.h"
#include "sdkconfig.h"
#include <bsp/esp-bsp.h>

#include "ui_service.h"
#include "ui_theme.h"
#include "ui.h"

#include "../scr_svc.h"
#include "../scr_ctrl.h"
#include "../scr_settings.h"

/* ---- shared helpers ------------------------------------------------------------*/
static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void back_settings_cb(lv_event_t *e)
{
    (void)e;
    ui_nav_open(UI_PAGE_SETTINGS);
}

static lv_obj_t *lbl(lv_obj_t *parent, const char *txt, const lv_font_t *f, uint32_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(col), 0);
    return l;
}

static lv_obj_t *card_at(lv_obj_t *parent, int x, int y, int w, int h, const char *title)
{
    lv_obj_t *c = ui_card(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_pad_all(c, 10, 0);
    if (title) {
        lv_obj_t *t = lbl(c, title, F_SM, UI_COL_ACCENT);
        lv_obj_set_style_text_letter_space(t, 3, 0);
        lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    }
    return c;
}

/* one-line key/value row at a fixed y inside a card */
static lv_obj_t *kv_at(lv_obj_t *card, int y, const char *key)
{
    lv_obj_t *k = lbl(card, key, F_SM, UI_COL_DIM);
    lv_obj_align(k, LV_ALIGN_TOP_LEFT, 0, y);
    lv_obj_t *v = lbl(card, "--", F_SM, UI_COL_TXT);
    lv_obj_set_size(v, LV_PCT(62), lv_font_get_line_height(F_SM));
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, y);
    return v;
}

static lv_obj_t *btn(lv_obj_t *parent, const char *txt, uint32_t bg, int w, int h)
{
    lv_obj_t *b = ui_button(parent, txt, lv_color_hex(bg), NULL, NULL);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_SURFACE), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, lv_color_hex(UI_COL_DIM), LV_STATE_DISABLED);
    return b;
}

static void set_enabled(lv_obj_t *o, bool en)
{
    bool dis = lv_obj_has_state(o, LV_STATE_DISABLED);
    if (en && dis) {
        lv_obj_remove_state(o, LV_STATE_DISABLED);
    } else if (!en && !dis) {
        lv_obj_add_state(o, LV_STATE_DISABLED);
    }
}

/* hold-to-confirm button: fires once after hold_ms of continuous press;
 * progress is shown on its own label by the page refresh */
typedef struct {
    lv_obj_t *b;
    lv_obj_t *l;
    const char *text;
    uint32_t hold_ms;
    int64_t t0;
    bool fired;
    void (*fn)(void);
} hold_t;

static void hold_ev(lv_event_t *e)
{
    hold_t *h = lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        h->t0 = now_ms();
        h->fired = false;
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        h->t0 = 0;
    }
}

static void hold_init(hold_t *h, lv_obj_t *parent, const char *text, uint32_t bg,
                      int w, int ht, uint32_t hold_ms, void (*fn)(void))
{
    h->b = btn(parent, text, bg, w, ht);
    h->l = lv_obj_get_child(h->b, 0);
    h->text = text;
    h->hold_ms = hold_ms;
    h->fn = fn;
    lv_obj_add_event_cb(h->b, hold_ev, LV_EVENT_PRESSED, h);
    lv_obj_add_event_cb(h->b, hold_ev, LV_EVENT_RELEASED, h);
    lv_obj_add_event_cb(h->b, hold_ev, LV_EVENT_PRESS_LOST, h);
}

static void hold_tick(hold_t *h)
{
    if (h->t0 != 0 && !h->fired && !lv_obj_has_state(h->b, LV_STATE_DISABLED)) {
        int64_t el = now_ms() - h->t0;
        if (el >= h->hold_ms) {
            h->fired = true;
            ui_label_set_text(h->l, h->text);
            h->fn();
            return;
        }
        uint32_t left = (uint32_t)(h->hold_ms - el);
        ui_label_set_fmt(h->l, "HOLD %lu.%lus", (unsigned long)(left / 1000u),
                         (unsigned long)(left % 1000u / 100u));
        return;
    }
    ui_label_set_text(h->l, h->text);
}

/* =================================================================================
 * FIRMWARE
 * ===============================================================================*/
typedef struct {
    lv_obj_t *stage, *ver, *size, *crc, *run, *why;
    hold_t go;
} fw_col_t;

static struct {
    fw_col_t col[SVC_FW_COUNT];
    lv_obj_t *phase, *bar, *tcbar, *tcpct, *msg;
    lv_obj_t *rescan;
} s_fw;

static void fw_go(svc_fw_t t)
{
    char why[64];
    if (!scr_svc_ota_start(t, why, sizeof(why))) {
        ui_toast("%s", why);
    } else {
        ui_toast("%s update started", t == SVC_FW_C6 ? "C6" : "TC275");
    }
}
static void fw_go_c6(void) { fw_go(SVC_FW_C6); }
static void fw_go_tc(void) { fw_go(SVC_FW_TC); }

static void fw_rescan_cb(lv_event_t *e)
{
    (void)e;
    scr_svc_rescan_stage();
    ui_toast("Re-scanning staging areas");
}

static void fw_col_create(lv_obj_t *root, svc_fw_t t, int x)
{
    fw_col_t *c = &s_fw.col[t];
    lv_obj_t *k = card_at(root, x, 58, 388, 250, t == SVC_FW_C6 ? "C6 GATEWAY" : "TC275 VEHICLE");
    lv_obj_t *ic = lbl(k, t == SVC_FW_C6 ? LV_SYMBOL_WIFI : LV_SYMBOL_DRIVE, F_LG, UI_COL_LINE);
    lv_obj_align(ic, LV_ALIGN_TOP_RIGHT, 0, -2);
    c->run   = kv_at(k, 26, "RUNNING");
    c->stage = kv_at(k, 50, "STAGED");
    c->ver   = kv_at(k, 74, "IMAGE VER");
    c->size  = kv_at(k, 98, "SIZE");
    c->crc   = kv_at(k, 122, "CRC32");
    c->why = lbl(k, " ", F_SM, UI_COL_WARN);
    lv_obj_set_size(c->why, LV_PCT(100), lv_font_get_line_height(F_SM));
    lv_label_set_long_mode(c->why, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(c->why, LV_ALIGN_TOP_LEFT, 0, 148);
    hold_init(&c->go, k, t == SVC_FW_C6 ? "UPDATE C6" : "UPDATE TC275", UI_COL_SURFACE2,
              LV_PCT(100), 48, 2000, t == SVC_FW_C6 ? fw_go_c6 : fw_go_tc);
    lv_obj_align(c->go.b, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void fw_create(lv_obj_t *root)
{
    ui_header(root, "FIRMWARE", back_settings_cb);
    fw_col_create(root, SVC_FW_C6, 8);
    fw_col_create(root, SVC_FW_TC, 404);

    lv_obj_t *p = card_at(root, 8, 316, 784, 156, "UPDATE STATUS");
    s_fw.phase = lbl(p, "IDLE", F_LG, UI_COL_TXT);
    lv_obj_align(s_fw.phase, LV_ALIGN_TOP_LEFT, 0, 22);
    lv_obj_t *l1 = lbl(p, "UPLOAD", F_SM, UI_COL_DIM);
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 0, 58);
    s_fw.bar = lv_bar_create(p);
    lv_obj_set_size(s_fw.bar, 560, 10);
    lv_obj_align(s_fw.bar, LV_ALIGN_TOP_LEFT, 80, 62);
    lv_obj_set_style_bg_color(s_fw.bar, lv_color_hex(UI_COL_SURFACE2), 0);
    lv_obj_set_style_bg_color(s_fw.bar, lv_color_hex(UI_COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_fw.bar, 2, 0);
    lv_obj_set_style_radius(s_fw.bar, 2, LV_PART_INDICATOR);
    lv_obj_t *l2 = lbl(p, "TC275", F_SM, UI_COL_DIM);
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 0, 82);
    s_fw.tcbar = lv_bar_create(p);
    lv_obj_set_size(s_fw.tcbar, 560, 10);
    lv_obj_align(s_fw.tcbar, LV_ALIGN_TOP_LEFT, 80, 86);
    lv_obj_set_style_bg_color(s_fw.tcbar, lv_color_hex(UI_COL_SURFACE2), 0);
    lv_obj_set_style_bg_color(s_fw.tcbar, lv_color_hex(UI_COL_OK), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_fw.tcbar, 2, 0);
    lv_obj_set_style_radius(s_fw.tcbar, 2, LV_PART_INDICATOR);
    s_fw.tcpct = lbl(p, "--", F_SM, UI_COL_TXT);
    lv_obj_align(s_fw.tcpct, LV_ALIGN_TOP_LEFT, 652, 82);
    s_fw.msg = lbl(p, "Stage images from a PC: tools/stage_fw.py --target c6|tc275",
                   F_SM, UI_COL_DIM);
    lv_obj_set_size(s_fw.msg, 640, lv_font_get_line_height(F_SM));
    lv_label_set_long_mode(s_fw.msg, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_fw.msg, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    s_fw.rescan = btn(p, LV_SYMBOL_REFRESH " RESCAN", UI_COL_SURFACE2, 120, 40);
    lv_obj_align(s_fw.rescan, LV_ALIGN_TOP_RIGHT, 0, 18);
    lv_obj_add_event_cb(s_fw.rescan, fw_rescan_cb, LV_EVENT_CLICKED, NULL);
}

void ui_service_fw_refresh(const scr_state_t *st)
{
    svc_ota_t o;
    scr_svc_get_ota(&o);
    bool busy = o.phase == SVC_OTA_VERIFY || o.phase == SVC_OTA_SEND || o.phase == SVC_OTA_WAIT_TC;
    bool link = st->conn == SCR_CONN_CONNECTED && st->ctrl_role;

    for (int t = 0; t < SVC_FW_COUNT; t++) {
        fw_col_t *c = &s_fw.col[t];
        svc_stage_t sg;
        scr_svc_get_stage((svc_fw_t)t, &sg);
        if (t == SVC_FW_C6) {
            ui_label_set_text(c->run, st->c6_fw[0] ? st->c6_fw : "--");
        } else if (st->tele_fresh && st->tc_fw_ver) {
            ui_label_set_fmt(c->run, "%u.%u.%u", (unsigned)((st->tc_fw_ver >> 16) & 0xFF),
                             (unsigned)((st->tc_fw_ver >> 8) & 0xFF),
                             (unsigned)(st->tc_fw_ver & 0xFF));
        } else {
            ui_label_set_text(c->run, "--");
        }
        static const char *const stxt[3] = { "EMPTY", "INVALID", "VERIFIED" };
        static const uint32_t scol[3] = { UI_COL_DIM, UI_COL_WARN, UI_COL_OK };
        ui_label_set_text(c->stage, stxt[sg.state]);
        ui_label_set_color(c->stage, lv_color_hex(scol[sg.state]));
        bool has = sg.state != SVC_STAGE_EMPTY && sg.size;
        ui_label_set_text(c->ver, has && sg.version[0] ? sg.version : "--");
        if (has) {
            ui_label_set_fmt(c->size, "%lu KB", (unsigned long)(sg.size / 1024u));
            ui_label_set_fmt(c->crc, "%08lX", (unsigned long)sg.crc32);
        } else {
            ui_label_set_text(c->size, "--");
            ui_label_set_text(c->crc, "--");
        }
        const char *why = " ";
        if (sg.state == SVC_STAGE_BAD) {
            why = sg.why;
        } else if (sg.state == SVC_STAGE_EMPTY) {
            why = "Nothing staged in this region";
        } else if (!link) {
            why = "Needs a live CTRL link to the car";
        } else if (t == SVC_FW_TC && !st->tc_on) {
            why = "TC275 link is down";
        }
        ui_label_set_text(c->why, why);
        bool can = !busy && sg.state == SVC_STAGE_OK && link &&
                   (t == SVC_FW_C6 || st->tc_on);
        set_enabled(c->go.b, can);
        hold_tick(&c->go);
    }
    set_enabled(s_fw.rescan, !busy);

    static const char *const ptxt[6] = { "IDLE", "VERIFYING IMAGE", "UPLOADING",
                                         "TC275 WRITING", "COMPLETE", "FAILED" };
    static const uint32_t pcol[6] = { UI_COL_DIM, UI_COL_INFO, UI_COL_ACCENT,
                                      UI_COL_INFO, UI_COL_OK, UI_COL_CRIT };
    if (o.phase == SVC_OTA_IDLE) {
        ui_label_set_text(s_fw.phase, "IDLE");
    } else {
        ui_label_set_fmt(s_fw.phase, "%s  -  %s", o.target == SVC_FW_C6 ? "C6" : "TC275",
                         ptxt[o.phase]);
    }
    ui_label_set_color(s_fw.phase, lv_color_hex(pcol[o.phase]));
    if (lv_bar_get_value(s_fw.bar) != o.pct) {
        lv_bar_set_value(s_fw.bar, o.pct, LV_ANIM_OFF);
    }
    int tp = (o.target == SVC_FW_TC && o.phase != SVC_OTA_IDLE) ? o.tc_pct : 0;
    if (lv_bar_get_value(s_fw.tcbar) != tp) {
        lv_bar_set_value(s_fw.tcbar, tp, LV_ANIM_OFF);
    }
    if (o.target == SVC_FW_TC && o.phase != SVC_OTA_IDLE) {
        ui_label_set_fmt(s_fw.tcpct, "%u%%", o.tc_pct);
    } else {
        ui_label_set_text(s_fw.tcpct, "--");
    }
    if (o.phase != SVC_OTA_IDLE) {
        ui_label_set_text(s_fw.msg, o.msg);
    }
}

/* =================================================================================
 * CALIBRATE
 * ===============================================================================*/
/* Rows are PHYSICAL positions (TC275 CALIB_POS_* order). The TC275 indexes
 * everything else (jog motor id, invert[], delta[], jog counts) by motor A..D,
 * so each row is resolved through the record's pos[] map: pos[motor] =
 * physical position. Until a record arrives, the TC275 factory map is assumed
 * (calib_record.c: A front-left, B rear-left, C rear-right, D front-right). */
static const char *const WHEEL[4] = { "FL", "FR", "RL", "RR" };
static const uint8_t DEF_POS[4] = { 0 /*A FL*/, 2 /*B RL*/, 3 /*C RR*/, 1 /*D FR*/ };
#define JOG_XTALK_CNT  20       /* |counts| on a wheel NOT being jogged = suspect */

static struct {
    lv_obj_t *pre[3];       /* precondition rows */
    lv_obj_t *off_sw;
    hold_t    dir;
    lv_obj_t *dir_state;
    lv_obj_t *res_name[4], *res_inv[4], *res_dl[4];
    lv_obj_t *rec_src, *rec_crc, *rec_map;
    lv_obj_t *fs_val, *wd_val;
    int16_t   fs, wd;
    bool      fs_wd_loaded;
    lv_obj_t *adj[4];       /* fs-, fs+, wd-, wd+ */
    lv_obj_t *write, *read;
    hold_t    clear;
    lv_obj_t *jog[8], *jog_name[4], *jog_cnt[4];
    uint8_t   motor_at[4];  /* physical position -> motor 0..3 (A..D) */
    int       jog_phys;     /* last pressed position, -1 = none */
    int       jog_dir;      /* +1 up / -1 down */
    bool      ok;           /* all preconditions met */
} s_cal;

/* Build position -> motor from pos[]; falls back to the factory map when the
 * record is missing or not a permutation of 0..3. */
static void cal_map_update(const svc_cal_t *c)
{
    const uint8_t *pos = DEF_POS;
    if (c->have_rec) {
        uint8_t seen = 0;
        for (int m = 0; m < 4; m++) {
            if (c->pos[m] < 4) {
                seen |= (uint8_t)(1u << c->pos[m]);
            }
        }
        if (seen == 0x0F) {
            pos = c->pos;
        }
    }
    for (int m = 0; m < 4; m++) {
        s_cal.motor_at[pos[m]] = (uint8_t)m;
    }
}

static void cal_dir_fire(void)
{
    if (!scr_svc_cal_start_dir()) {
        ui_toast("Send failed - link?");
    }
}

static void cal_clear_fire(void)
{
    if (scr_svc_cal_rec_clear()) {
        s_cal.fs_wd_loaded = false;
        ui_toast("Defaults restored");
    }
}

static void cal_read_cb(lv_event_t *e)
{
    (void)e;
    s_cal.fs_wd_loaded = false;
    (void)scr_svc_cal_rec_get();
}

static void cal_write_cb(lv_event_t *e)
{
    (void)e;
    if (scr_svc_cal_rec_set(s_cal.fs, s_cal.wd)) {
        ui_toast("Written: fullScale %d, wheel %d mm", s_cal.fs, s_cal.wd);
        s_cal.fs_wd_loaded = false;
    } else {
        ui_toast("Read the record first");
    }
}

static void cal_adj_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 2) {
        int v = s_cal.fs + (i == 0 ? -50 : 50);
        s_cal.fs = (int16_t)(v < 100 ? 100 : (v > 5000 ? 5000 : v));
    } else {
        int v = s_cal.wd + (i == 2 ? -1 : 1);
        s_cal.wd = (int16_t)(v < 30 ? 30 : (v > 200 ? 200 : v));
    }
}

static void jog_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED && s_cal.ok) {
        s_cal.jog_phys = i / 2;
        s_cal.jog_dir = (i & 1) ? 1 : -1;
        scr_ctrl_set_jog(s_cal.motor_at[i / 2], (i & 1) ? 300 : -300);
    } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        scr_ctrl_set_jog(-1, 0);
    }
}

/* Solid ▲ / ▼ on the jog buttons. Montserrat has no U+25B2/25BC glyphs, so
 * the triangle is drawn directly; it takes the button's current text color,
 * so the disabled (dim) state greys it out like a label would. */
static void jog_arrow_draw_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target_obj(e);
    bool up = (intptr_t)lv_event_get_user_data(e) != 0;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t cx = (a.x1 + a.x2) / 2;
    int32_t cy = (a.y1 + a.y2) / 2;
    const int32_t hw = 14, hh = 12;     /* 28 x 24 px triangle */

    lv_draw_triangle_dsc_t dsc;
    lv_draw_triangle_dsc_init(&dsc);
    dsc.color = lv_obj_get_style_text_color(o, LV_PART_MAIN);
    dsc.opa = LV_OPA_COVER;
    dsc.p[0].x = cx;
    dsc.p[0].y = up ? cy - hh : cy + hh;
    dsc.p[1].x = cx - hw;
    dsc.p[1].y = up ? cy + hh : cy - hh;
    dsc.p[2].x = cx + hw;
    dsc.p[2].y = dsc.p[1].y;
    lv_draw_triangle(lv_event_get_layer(e), &dsc);
}

static void calib_create(lv_obj_t *root)
{
    ui_header(root, "CALIBRATE  TC275", back_settings_cb);
    {
        svc_cal_t none = { 0 };
        cal_map_update(&none);          /* factory map until a record arrives */
        s_cal.jog_phys = -1;
    }

    /* preconditions */
    lv_obj_t *pc = card_at(root, 8, 58, 240, 250, "PRECONDITIONS");
    static const char *const pre[3] = { "CTRL LINK", "TC275 ONLINE", "WHEELS OFF GROUND" };
    for (int i = 0; i < 3; i++) {
        s_cal.pre[i] = lbl(pc, pre[i], F_SM, UI_COL_DIM);
        lv_obj_align(s_cal.pre[i], LV_ALIGN_TOP_LEFT, 0, 30 + i * 30);
    }
    s_cal.off_sw = lv_switch_create(pc);
    lv_obj_set_size(s_cal.off_sw, 56, 28);
    lv_obj_align(s_cal.off_sw, LV_ALIGN_TOP_RIGHT, 0, 84);
    lv_obj_set_style_bg_color(s_cal.off_sw, lv_color_hex(UI_COL_ACCENT),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_t *note = lbl(pc, "Lift the car so all four\nwheels spin freely before\nany calibration or jog.",
                         F_SM, UI_COL_DIM);
    lv_obj_align(note, LV_ALIGN_TOP_LEFT, 0, 132);
    lv_obj_set_style_text_line_space(note, 3, 0);

    /* encoder direction */
    lv_obj_t *dc = card_at(root, 256, 58, 268, 250, "ENCODER DIRECTION");
    s_cal.dir_state = lbl(dc, "Not run", F_SM, UI_COL_DIM);
    lv_obj_set_size(s_cal.dir_state, LV_PCT(100), lv_font_get_line_height(F_SM));
    lv_label_set_long_mode(s_cal.dir_state, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_cal.dir_state, LV_ALIGN_TOP_LEFT, 0, 24);
    lv_obj_t *h1 = lbl(dc, "WHEEL", F_SM, UI_COL_DIM);
    lv_obj_align(h1, LV_ALIGN_TOP_LEFT, 0, 50);
    lv_obj_t *h2 = lbl(dc, "DIR", F_SM, UI_COL_DIM);
    lv_obj_align(h2, LV_ALIGN_TOP_LEFT, 80, 50);
    lv_obj_t *h3 = lbl(dc, "COUNTS", F_SM, UI_COL_DIM);
    lv_obj_align(h3, LV_ALIGN_TOP_RIGHT, 0, 50);
    for (int i = 0; i < 4; i++) {
        s_cal.res_name[i] = lbl(dc, WHEEL[i], F_MD, UI_COL_TXT);
        lv_obj_align(s_cal.res_name[i], LV_ALIGN_TOP_LEFT, 0, 72 + i * 24);
        s_cal.res_inv[i] = lbl(dc, "--", F_SM, UI_COL_DIM);
        lv_obj_align(s_cal.res_inv[i], LV_ALIGN_TOP_LEFT, 80, 74 + i * 24);
        s_cal.res_dl[i] = lbl(dc, "--", F_SM, UI_COL_DIM);
        lv_obj_align(s_cal.res_dl[i], LV_ALIGN_TOP_RIGHT, 0, 74 + i * 24);
    }
    hold_init(&s_cal.dir, dc, "RUN (HOLD 1.5s)", UI_COL_SURFACE2, LV_PCT(100), 40, 1500, cal_dir_fire);
    lv_obj_align(s_cal.dir.b, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* calibration record */
    lv_obj_t *rc = card_at(root, 532, 58, 260, 250, "RECORD");
    s_cal.rec_src = kv_at(rc, 24, "SOURCE");
    s_cal.rec_crc = kv_at(rc, 46, "CRC");
    s_cal.rec_map = kv_at(rc, 68, "INVERT FL FR RL RR");
    lv_obj_t *fk = lbl(rc, "FULL SCALE", F_SM, UI_COL_DIM);
    lv_obj_align(fk, LV_ALIGN_TOP_LEFT, 0, 98);
    lv_obj_t *wk = lbl(rc, "WHEEL mm", F_SM, UI_COL_DIM);
    lv_obj_align(wk, LV_ALIGN_TOP_LEFT, 0, 136);
    for (int i = 0; i < 4; i++) {
        s_cal.adj[i] = btn(rc, (i & 1) ? LV_SYMBOL_PLUS : LV_SYMBOL_MINUS, UI_COL_SURFACE2, 36, 32);
        lv_obj_align(s_cal.adj[i], LV_ALIGN_TOP_RIGHT, (i & 1) ? 0 : -96, i < 2 ? 90 : 128);
        lv_obj_add_event_cb(s_cal.adj[i], cal_adj_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    s_cal.fs_val = lbl(rc, "--", F_MD, UI_COL_TXT);
    lv_obj_set_width(s_cal.fs_val, 56);
    lv_obj_set_style_text_align(s_cal.fs_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_cal.fs_val, LV_ALIGN_TOP_RIGHT, -38, 96);
    s_cal.wd_val = lbl(rc, "--", F_MD, UI_COL_TXT);
    lv_obj_set_width(s_cal.wd_val, 56);
    lv_obj_set_style_text_align(s_cal.wd_val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_cal.wd_val, LV_ALIGN_TOP_RIGHT, -38, 134);
    s_cal.read = btn(rc, LV_SYMBOL_DOWNLOAD " READ", UI_COL_SURFACE2, 114, 36);
    lv_obj_align(s_cal.read, LV_ALIGN_BOTTOM_LEFT, 0, -42);
    lv_obj_add_event_cb(s_cal.read, cal_read_cb, LV_EVENT_CLICKED, NULL);
    s_cal.write = btn(rc, LV_SYMBOL_SAVE " WRITE", UI_COL_SURFACE2, 114, 36);
    lv_obj_align(s_cal.write, LV_ALIGN_BOTTOM_RIGHT, 0, -42);
    lv_obj_add_event_cb(s_cal.write, cal_write_cb, LV_EVENT_CLICKED, NULL);
    hold_init(&s_cal.clear, rc, "DEFAULTS (HOLD)", UI_COL_SURFACE2, LV_PCT(100), 36, 1500, cal_clear_fire);
    lv_obj_align(s_cal.clear.b, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* motor jog */
    lv_obj_t *jc = card_at(root, 8, 316, 784, 156, "MOTOR JOG  -  HOLD TO SPIN 30%, RELEASE TO STOP");
    for (int m = 0; m < 4; m++) {
        int x = m * 192;
        s_cal.jog_name[m] = lbl(jc, WHEEL[m], F_LG, UI_COL_TXT);
        lv_obj_align(s_cal.jog_name[m], LV_ALIGN_TOP_LEFT, x, 30);
        /* live encoder counts for this wheel since the current press */
        s_cal.jog_cnt[m] = lbl(jc, "--", F_MD, UI_COL_DIM);
        lv_obj_set_width(s_cal.jog_cnt[m], 84);
        lv_obj_set_style_text_align(s_cal.jog_cnt[m], LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(s_cal.jog_cnt[m], LV_LABEL_LONG_MODE_CLIP);
        lv_obj_align(s_cal.jog_cnt[m], LV_ALIGN_TOP_LEFT, x + 84, 34);
        for (int d = 0; d < 2; d++) {
            int i = m * 2 + d;
            s_cal.jog[i] = btn(jc, "", UI_COL_SURFACE2, 80, 64);
            /* d=1 -> +duty (forward) ▲, d=0 -> -duty ▼ */
            lv_obj_add_event_cb(s_cal.jog[i], jog_arrow_draw_cb, LV_EVENT_DRAW_MAIN_END,
                                (void *)(intptr_t)d);
            lv_obj_align(s_cal.jog[i], LV_ALIGN_TOP_LEFT, x + d * 88, 62);
            lv_obj_set_style_bg_color(s_cal.jog[i], lv_color_hex(UI_COL_ACCENT), LV_STATE_PRESSED);
            lv_obj_add_event_cb(s_cal.jog[i], jog_cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(s_cal.jog[i], jog_cb, LV_EVENT_RELEASED, (void *)(intptr_t)i);
            lv_obj_add_event_cb(s_cal.jog[i], jog_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)i);
        }
    }
}

static void pre_row(int i, bool ok)
{
    static const char *const base[3] = { "CTRL LINK", "TC275 ONLINE", "WHEELS OFF GROUND" };
    ui_label_set_fmt(s_cal.pre[i], "%s  %s", ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, base[i]);
    ui_label_set_color(s_cal.pre[i], lv_color_hex(ok ? UI_COL_OK : UI_COL_WARN));
}

void ui_service_calib_refresh(const scr_state_t *st)
{
    bool link = st->conn == SCR_CONN_CONNECTED && st->ctrl_role;
    bool tc = link && st->tc_on;
    bool off = lv_obj_has_state(s_cal.off_sw, LV_STATE_CHECKED);
    pre_row(0, link);
    pre_row(1, tc);
    pre_row(2, off);
    s_cal.ok = link && tc && off && !st->emerg_latch;
    if (!s_cal.ok) {
        scr_ctrl_set_jog(-1, 0);        /* a precondition dropped mid-jog */
    }

    svc_cal_t c;
    scr_svc_get_cal(&c);
    cal_map_update(&c);
    for (int i = 0; i < 4; i++) {
        char l = (char)('A' + s_cal.motor_at[i]);
        ui_label_set_fmt(s_cal.res_name[i], "%s %c", WHEEL[i], l);
        ui_label_set_fmt(s_cal.jog_name[i], "%s (%c)", WHEEL[i], l);
    }

    /* live jog counts: the pressed wheel must move with the button's sign,
     * the other three must stay ~0. Green = as expected, red = pressed wheel
     * wrong sign / no count, amber = a wheel that was NOT jogged moved. */
    for (int i = 0; i < 4; i++) {
        if (!c.have_jog || s_cal.jog_phys < 0) {
            continue;
        }
        int32_t d = c.jog_d[s_cal.motor_at[i]];
        uint32_t col;
        if (i == s_cal.jog_phys) {
            bool good = (int64_t)d * s_cal.jog_dir > JOG_XTALK_CNT;
            col = good ? UI_COL_OK : UI_COL_CRIT;
        } else {
            col = (d > JOG_XTALK_CNT || d < -JOG_XTALK_CNT) ? UI_COL_WARN : UI_COL_DIM;
        }
        ui_label_set_fmt(s_cal.jog_cnt[i], "%+ld", (long)d);
        ui_label_set_color(s_cal.jog_cnt[i], lv_color_hex(col));
    }

    /* direction run */
    if (c.cal_started_ms && now_ms() - c.cal_started_ms > 3000) {
        ui_label_set_text(s_cal.dir_state, "No reply in 3 s - check TC275 serial ENCCAL=");
        ui_label_set_color(s_cal.dir_state, lv_color_hex(UI_COL_WARN));
    } else if (c.cal_started_ms) {
        ui_label_set_text(s_cal.dir_state, "Running - wheels will turn briefly");
        ui_label_set_color(s_cal.dir_state, lv_color_hex(UI_COL_INFO));
    } else if (c.have_cal) {
        static const char *const cs[3] = { "DONE", "ABORTED (E-STOP)", "BUSY" };
        static const char *const sv[3] = { "not saved", "saved", "SAVE FAILED" };
        ui_label_set_fmt(s_cal.dir_state, "%s  -  %s", c.status <= 2 ? cs[c.status] : "?",
                         c.saved >= 0 && c.saved <= 2 ? sv[c.saved] : "save n/a");
        ui_label_set_color(s_cal.dir_state,
                           lv_color_hex(c.status == 0 && c.saved != 2 ? UI_COL_OK : UI_COL_WARN));
    }
    for (int i = 0; i < 4; i++) {
        int m = s_cal.motor_at[i];      /* row i = physical position i */
        if (c.have_cal) {
            bool nocount = c.delta[m] == 0;
            ui_label_set_text(s_cal.res_inv[i], nocount ? "NO COUNT" :
                              (c.invert[m] < 0 ? "INVERTED" : "NORMAL"));
            ui_label_set_color(s_cal.res_inv[i], lv_color_hex(nocount ? UI_COL_CRIT :
                               (c.invert[m] < 0 ? UI_COL_WARN : UI_COL_OK)));
            ui_label_set_fmt(s_cal.res_dl[i], "%ld", (long)c.delta[m]);
        }
    }
    set_enabled(s_cal.dir.b, s_cal.ok && c.cal_started_ms == 0);
    hold_tick(&s_cal.dir);

    /* record */
    if (c.have_rec) {
        static const char *const src[3] = { "DEFAULT", "DFLASH", "ONLINE" };
        ui_label_set_text(s_cal.rec_src, c.rec_src <= 2 ? src[c.rec_src] : "?");
        ui_label_set_text(s_cal.rec_crc, c.rec_crc_ok ? "OK" : "BAD");
        ui_label_set_color(s_cal.rec_crc, lv_color_hex(c.rec_crc_ok ? UI_COL_OK : UI_COL_CRIT));
        ui_label_set_fmt(s_cal.rec_map, "%c%c%c%c",
                         c.rec_invert[s_cal.motor_at[0]] < 0 ? '-' : '+',
                         c.rec_invert[s_cal.motor_at[1]] < 0 ? '-' : '+',
                         c.rec_invert[s_cal.motor_at[2]] < 0 ? '-' : '+',
                         c.rec_invert[s_cal.motor_at[3]] < 0 ? '-' : '+');
        if (!s_cal.fs_wd_loaded) {
            s_cal.fs = c.full_scale;
            s_cal.wd = c.wheel_dia;
            s_cal.fs_wd_loaded = true;
        }
        ui_label_set_fmt(s_cal.fs_val, "%d", s_cal.fs);
        ui_label_set_fmt(s_cal.wd_val, "%d", s_cal.wd);
    } else {
        ui_label_set_text(s_cal.rec_src, link ? "reading..." : "--");
    }
    bool rec_ok = link && tc;
    set_enabled(s_cal.read, rec_ok);
    set_enabled(s_cal.write, rec_ok && c.have_rec &&
                (s_cal.fs != c.full_scale || s_cal.wd != c.wheel_dia));
    for (int i = 0; i < 4; i++) {
        set_enabled(s_cal.adj[i], rec_ok && c.have_rec);
    }
    set_enabled(s_cal.clear.b, rec_ok);
    hold_tick(&s_cal.clear);
    for (int i = 0; i < 8; i++) {
        set_enabled(s_cal.jog[i], s_cal.ok);
    }
}

/* =================================================================================
 * DIAGNOSE (C6 + TC275)
 * ===============================================================================*/
#define FIND_MAX 8

static struct {
    lv_obj_t *verdict, *vsub;
    lv_obj_t *find[FIND_MAX];
    lv_obj_t *c6[12];
    lv_obj_t *tc[13];
    lv_obj_t *refresh, *clear;
    int64_t trk_bad_since[2];
} s_fd;

static void fd_refresh_cb(lv_event_t *e)
{
    (void)e;
    scr_svc_diag_poll_now();
    ui_toast("Polling C6 diagnostics");
}

static void fd_clear_cb(lv_event_t *e)
{
    (void)e;
    ui_toast(scr_svc_clear_fault() ? "Clear-fault sent (TC275 decides)" : "Send failed - link?");
}

static void fdiag_create(lv_obj_t *root)
{
    ui_header(root, "DIAGNOSE  C6 / TC275", back_settings_cb);

    lv_obj_t *hc = card_at(root, 8, 58, 300, 414, "HEALTH");
    s_fd.verdict = lbl(hc, "CHECKING", F_XL, UI_COL_DIM);
    lv_obj_align(s_fd.verdict, LV_ALIGN_TOP_LEFT, 0, 22);
    s_fd.vsub = lbl(hc, " ", F_SM, UI_COL_DIM);
    lv_obj_align(s_fd.vsub, LV_ALIGN_TOP_LEFT, 0, 58);
    for (int i = 0; i < FIND_MAX; i++) {
        s_fd.find[i] = lbl(hc, " ", F_SM, UI_COL_TXT);
        lv_obj_set_size(s_fd.find[i], LV_PCT(100), lv_font_get_line_height(F_SM) * 2 + 2);
        lv_label_set_long_mode(s_fd.find[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(s_fd.find[i], LV_ALIGN_TOP_LEFT, 0, 86 + i * 38);
    }
    s_fd.refresh = btn(hc, LV_SYMBOL_REFRESH " REFRESH", UI_COL_SURFACE2, 132, 38);
    lv_obj_align(s_fd.refresh, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(s_fd.refresh, fd_refresh_cb, LV_EVENT_CLICKED, NULL);
    s_fd.clear = btn(hc, "CLEAR FAULT", UI_COL_SURFACE2, 132, 38);
    lv_obj_align(s_fd.clear, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(s_fd.clear, fd_clear_cb, LV_EVENT_CLICKED, NULL);

    static const char *const c6k[12] = { "FIRMWARE", "STATE", "SLOT", "LAST RESET",
                                         "SELF-CHECK", "COREDUMP", "HEAP MIN", "UPTIME",
                                         "SPI LINK", "LINK RTT", "CRC / FMT ERR", "CLIENTS / IMU" };
    lv_obj_t *cc = card_at(root, 316, 58, 234, 414, "C6 GATEWAY");
    for (int i = 0; i < 12; i++) {
        s_fd.c6[i] = kv_at(cc, 26 + i * 31, c6k[i]);
    }
    static const char *const tck[13] = { "TELEMETRY", "STATE", "FAULT", "FIRMWARE",
                                         "HW REV", "BATTERY", "LEFT T/M", "RIGHT T/M",
                                         "ODO SESSION", "ODO TOTAL", "LINK RTT", "LINK ERR",
                                         "SBL" };
    lv_obj_t *tcc = card_at(root, 558, 58, 234, 414, "TC275 VEHICLE");
    for (int i = 0; i < 13; i++) {
        s_fd.tc[i] = kv_at(tcc, 26 + i * 29, tck[i]);
    }
}

static const char *reset_name(uint32_t r)
{
    static const char *const n[9] = { "unknown", "power-on", "software", "PANIC", "INT WDT",
                                      "TASK WDT", "RTC WDT", "BROWNOUT", "BROWNOUT" };
    return r < 9 ? n[r] : "other";
}

typedef struct {
    int n;
    int worst;          /* 0 ok, 1 warn, 2 crit */
    char txt[FIND_MAX][80];
    int sev[FIND_MAX];
} findings_t;

static void add_find(findings_t *f, int sev, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void add_find(findings_t *f, int sev, const char *fmt, ...)
{
    if (f->n >= FIND_MAX) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->txt[f->n], sizeof(f->txt[0]), fmt, ap);
    va_end(ap);
    f->sev[f->n++] = sev;
    if (sev > f->worst) {
        f->worst = sev;
    }
}

void ui_service_fdiag_refresh(const scr_state_t *st)
{
    svc_c6diag_t d;
    scr_svc_get_c6diag(&d);
    bool link = st->conn == SCR_CONN_CONNECTED;
    findings_t f = { 0 };

    /* ---- derive findings (doc/08 §6) ---- */
    if (!link) {
        add_find(&f, 2, "No Wi-Fi/WS link to the C6\nCheck car power and range");
    } else if (!d.valid && d.age_ms != -1 && d.age_ms > 6000) {
        add_find(&f, 2, "C6 diagnostics not answering\nC6 busy or rebooting");
    }
    if (d.valid) {
        if (d.reset >= 3 && d.reset <= 8) {
            add_find(&f, 1, "C6 last reset: %s\nCheck supply / C6 log", reset_name(d.reset));
        }
        if (d.coredump) {
            add_find(&f, 1, "C6 holds a crash dump\nRead it: idf.py coredump-info");
        }
        if (!d.link_up) {
            add_find(&f, 2, "C6 <-> TC275 SPI link DOWN\nCheck TC275 power / SPI cable");
        } else if (d.crc_err_delta || d.fmt_err_delta) {
            add_find(&f, 1, "SPI errors rising (+%lu CRC +%lu FMT)\nCheck wiring / EMI",
                     (unsigned long)d.crc_err_delta, (unsigned long)d.fmt_err_delta);
        }
        if (d.heap_min && d.heap_min < 64u * 1024u) {
            add_find(&f, 1, "C6 memory low (min %lu KB)\nToo many clients?",
                     (unsigned long)(d.heap_min / 1024u));
        }
        if (d.imu == 0) {
            add_find(&f, 1, "C6 IMU (ADXL345) error\nCheck sensor wiring");
        }
    }
    if (link && st->tc_on && !st->tele_fresh) {
        add_find(&f, 2, "TC275 telemetry stale\nTC275 stalled or link saturated");
    }
    if (st->tele_fresh) {
        if (st->fault_code) {
            add_find(&f, 2, "TC275 fault 0x%04X\nSee tc275_car fault table", st->fault_code);
        }
        if (st->batt_pct && st->batt_pct <= CONFIG_SCR_BATT_LOW_PCT) {
            add_find(&f, st->batt_pct <= CONFIG_SCR_BATT_CRIT_PCT ? 2 : 1,
                     "Battery %u%% (%u mV)\nCharge before driving", st->batt_pct, st->batt_mv);
        }
        int16_t tg[2] = { st->v_target_l, st->v_target_r };
        int16_t ms[2] = { st->v_meas_l, st->v_meas_r };
        for (int i = 0; i < 2; i++) {
            if (abs(tg[i] - ms[i]) > 200) {
                if (s_fd.trk_bad_since[i] == 0) {
                    s_fd.trk_bad_since[i] = now_ms();
                } else if (now_ms() - s_fd.trk_bad_since[i] > 1500) {
                    add_find(&f, 1, "%s side speed tracking off\nMotor/encoder: use CALIBRATE",
                             i ? "RIGHT" : "LEFT");
                }
            } else {
                s_fd.trk_bad_since[i] = 0;
            }
        }
    } else {
        s_fd.trk_bad_since[0] = s_fd.trk_bad_since[1] = 0;
    }

    static const uint32_t sevc[3] = { UI_COL_OK, UI_COL_WARN, UI_COL_CRIT };
    static const char *const vtxt[3] = { "NOMINAL", "ATTENTION", "FAULT" };
    ui_label_set_text(s_fd.verdict, vtxt[f.worst]);
    ui_label_set_color(s_fd.verdict, lv_color_hex(sevc[f.worst]));
    if (f.n == 0) {
        ui_label_set_text(s_fd.vsub, "All systems nominal");
    } else {
        ui_label_set_fmt(s_fd.vsub, "%d finding%s", f.n, f.n > 1 ? "s" : "");
    }
    for (int i = 0; i < FIND_MAX; i++) {
        if (i < f.n) {
            char b[96];
            snprintf(b, sizeof(b), "%s %s", f.sev[i] == 2 ? LV_SYMBOL_WARNING : LV_SYMBOL_BELL, f.txt[i]);
            ui_label_set_text(s_fd.find[i], b);
            ui_label_set_color(s_fd.find[i], lv_color_hex(sevc[f.sev[i]]));
        } else {
            ui_label_set_text(s_fd.find[i], " ");
        }
    }

    /* ---- C6 details ---- */
    if (d.valid) {
        ui_label_set_text(s_fd.c6[0], d.ver);
        ui_label_set_text(s_fd.c6[1], d.state);
        ui_label_set_fmt(s_fd.c6[2], "%s%s", d.slot, d.factory ? " (F)" : "");
        ui_label_set_text(s_fd.c6[3], reset_name(d.reset));
        ui_label_set_color(s_fd.c6[3], lv_color_hex(d.reset >= 3 && d.reset <= 8 ? UI_COL_WARN : UI_COL_TXT));
        ui_label_set_fmt(s_fd.c6[4], "0x%lx", (unsigned long)d.selfcheck);
        ui_label_set_text(s_fd.c6[5], d.coredump ? "PRESENT" : "none");
        ui_label_set_color(s_fd.c6[5], lv_color_hex(d.coredump ? UI_COL_WARN : UI_COL_TXT));
        ui_label_set_fmt(s_fd.c6[6], "%lu KB", (unsigned long)(d.heap_min / 1024u));
        ui_label_set_fmt(s_fd.c6[7], "%lu:%02lu:%02lu", (unsigned long)(d.uptime_s / 3600u),
                         (unsigned long)(d.uptime_s / 60u % 60u), (unsigned long)(d.uptime_s % 60u));
        ui_label_set_text(s_fd.c6[8], d.link_up ? "UP" : "DOWN");
        ui_label_set_color(s_fd.c6[8], lv_color_hex(d.link_up ? UI_COL_OK : UI_COL_CRIT));
        ui_label_set_fmt(s_fd.c6[9], "%lu ms", (unsigned long)d.link_rtt);
        ui_label_set_fmt(s_fd.c6[10], "%lu / %lu", (unsigned long)d.crc_err, (unsigned long)d.fmt_err);
        ui_label_set_color(s_fd.c6[10], lv_color_hex(d.crc_err_delta || d.fmt_err_delta ? UI_COL_WARN : UI_COL_TXT));
        ui_label_set_fmt(s_fd.c6[11], "%d / %s", d.cli, d.imu < 0 ? "n/a" : (d.imu ? "ok" : "ERR"));
    } else {
        for (int i = 0; i < 12; i++) {
            ui_label_set_text(s_fd.c6[i], "--");
            ui_label_set_color(s_fd.c6[i], lv_color_hex(UI_COL_DIM));
        }
    }

    /* ---- TC275 details ---- */
    bool tf = st->tele_fresh;
    ui_label_set_text(s_fd.tc[0], tf ? "LIVE" : (st->tc_on ? "STALE" : "OFFLINE"));
    ui_label_set_color(s_fd.tc[0], lv_color_hex(tf ? UI_COL_OK : UI_COL_CRIT));
    if (tf) {
        ui_label_set_fmt(s_fd.tc[1], "%u", st->veh_state);
        ui_label_set_fmt(s_fd.tc[2], st->fault_code ? "0x%04X" : "none", st->fault_code);
        ui_label_set_color(s_fd.tc[2], lv_color_hex(st->fault_code ? UI_COL_CRIT : UI_COL_TXT));
        ui_label_set_fmt(s_fd.tc[3], "%u.%u.%u", (unsigned)((st->tc_fw_ver >> 16) & 0xFF),
                         (unsigned)((st->tc_fw_ver >> 8) & 0xFF), (unsigned)(st->tc_fw_ver & 0xFF));
        ui_label_set_fmt(s_fd.tc[4], "%u", st->hw_rev);
        ui_label_set_fmt(s_fd.tc[5], "%u%%  %u.%02uV", st->batt_pct, st->batt_mv / 1000u,
                         (st->batt_mv % 1000u) / 10u);
        ui_label_set_fmt(s_fd.tc[6], "%d / %d", st->v_target_l, st->v_meas_l);
        ui_label_set_fmt(s_fd.tc[7], "%d / %d", st->v_target_r, st->v_meas_r);
        ui_label_set_fmt(s_fd.tc[8], "%lu m", (unsigned long)(st->odo_session_mm / 1000u));
        ui_label_set_fmt(s_fd.tc[9], "%lu m", (unsigned long)(st->odo_total_mm / 1000u));
        ui_label_set_fmt(s_fd.tc[10], "%u ms", st->link_rtt_ms);
        ui_label_set_fmt(s_fd.tc[11], "%u.%u%%", st->link_err_rate / 10u, st->link_err_rate % 10u);
    } else {
        for (int i = 1; i < 12; i++) {
            ui_label_set_text(s_fd.tc[i], "--");
            ui_label_set_color(s_fd.tc[i], lv_color_hex(UI_COL_DIM));
        }
    }

    /* SBL version rides its own beacon ({"t":"tcver"}), independent of
     * telemetry freshness; empty string = SBL not flashed / pre-version SBL. */
    if (st->tc_sbl_ver[0] == '\0') {
        ui_label_set_text(s_fd.tc[12], "--");
        ui_label_set_color(s_fd.tc[12], lv_color_hex(UI_COL_DIM));
    } else if (strncmp(st->tc_sbl_ver, "SBLFW tc275_sbl v", 17) == 0) {
        ui_label_set_text(s_fd.tc[12], st->tc_sbl_ver + 17);
        ui_label_set_color(s_fd.tc[12], lv_color_hex(UI_COL_TXT));
    } else {
        ui_label_set_text(s_fd.tc[12], st->tc_sbl_ver);
        ui_label_set_color(s_fd.tc[12], lv_color_hex(UI_COL_TXT));
    }
    set_enabled(s_fd.clear, link && st->ctrl_role);
}

/* =================================================================================
 * lifecycle
 * ===============================================================================*/
void ui_service_create(lv_obj_t *fw, lv_obj_t *calib, lv_obj_t *fdiag)
{
    memset(&s_fw, 0, sizeof(s_fw));
    memset(&s_cal, 0, sizeof(s_cal));
    memset(&s_fd, 0, sizeof(s_fd));
    fw_create(fw);
    calib_create(calib);
    fdiag_create(fdiag);
}

void ui_service_on_enter(ui_page_t p)
{
    if (p == UI_PAGE_FDIAG) {
        scr_svc_diag_poll_enable(true);
        scr_svc_diag_poll_now();
    } else if (p == UI_PAGE_CALIB) {
        s_cal.fs_wd_loaded = false;
        (void)scr_svc_cal_rec_get();
    } else if (p == UI_PAGE_FW) {
        scr_svc_rescan_stage();
    }
}

void ui_service_on_leave(ui_page_t p)
{
    if (p == UI_PAGE_FDIAG) {
        scr_svc_diag_poll_enable(false);
    } else if (p == UI_PAGE_CALIB) {
        scr_ctrl_set_jog(-1, 0);
        lv_obj_remove_state(s_cal.off_sw, LV_STATE_CHECKED);   /* re-confirm next time */
    }
}
