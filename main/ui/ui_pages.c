#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "sdkconfig.h"
#include <bsp/esp-bsp.h>

#include "ui_pages.h"
#include "ui_theme.h"
#include "ui.h"

#include "../app_state.h"
#include "../scr_settings.h"
#include "../scr_link.h"
#include "../proto/proto_frames.h"

/* ---- shared page scaffolding ---------------------------------------------------*/

/* settings sub panels; *_NAV_* entries jump to their own pages */
typedef enum {
    SUB_NONE = 0,
    SUB_CONTROL,
    SUB_RADIO,
    SUB_DISPLAY,
    SUB_ABOUT,
    SUB_NAV_PAIR,   /* jumps to the Pairing page   */
    SUB_NAV_DIAG,   /* jumps to Diagnostics        */
    SUB_NAV_EVENTS, /* jumps to the Event log      */
} sub_t;

static void set_open_sub(sub_t s);

static void nav_home_cb(lv_event_t *e)
{
    ui_nav_open(UI_PAGE_HOME);
}

/* scrollable key/value body under a 52 px header */
static lv_obj_t *kv_body(lv_obj_t *root)
{
    lv_obj_t *body = lv_obj_create(root);
    lv_obj_set_size(body, LV_PCT(100), bsp_display_get_v_res() - 66);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 58);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_pad_hor(body, 8, 0);
    lv_obj_set_style_pad_ver(body, 4, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    return body;
}

static lv_obj_t *kv_section(lv_obj_t *body, const char *title)
{
    lv_obj_t *l = lv_label_create(body);
    ui_label_set_text(l, title);
    ui_label_set_color(l, lv_color_hex(UI_COL_ACCENT));
    lv_obj_set_style_text_font(l, F_SM, 0);
    return l;
}

/* ---- Vehicle page (spec 23) ------------------------------------------------------*/

typedef struct {
    lv_obj_t *state, *speed, *thr, *str, *mode, *batt, *battv, *odo, *fault, *tcfw, *hw;
} veh_t;

static veh_t s_veh;

void ui_pages_create_vehicle(lv_obj_t *root)
{
    ui_header(root, "VEHICLE", nav_home_cb);
    lv_obj_t *body = kv_body(root);
    kv_section(body, "DRIVE");
    ui_kv_row(body, "State", &s_veh.state);
    ui_kv_row(body, "Speed", &s_veh.speed);
    ui_kv_row(body, "Throttle", &s_veh.thr);
    ui_kv_row(body, "Steering", &s_veh.str);
    ui_kv_row(body, "Mode", &s_veh.mode);
    kv_section(body, "POWER");
    ui_kv_row(body, "Battery", &s_veh.batt);
    ui_kv_row(body, "Battery voltage", &s_veh.battv);
    ui_kv_row(body, "Odometer (session)", &s_veh.odo);
    kv_section(body, "VEHICLE CORE");
    ui_kv_row(body, "Fault", &s_veh.fault);
    ui_kv_row(body, "TC275 firmware", &s_veh.tcfw);
    ui_kv_row(body, "HW revision", &s_veh.hw);
}

void ui_pages_vehicle_refresh(const scr_state_t *st)
{
    if (st->tele_fresh) {
        ui_label_set_fmt(s_veh.state, "0x%02X%s", st->veh_state, st->tc_on ? "" : " (no link)");
        int32_t sp = st->speed_mm_s;
        int32_t sa = sp < 0 ? -sp : sp;
        ui_label_set_fmt(s_veh.speed, "%c%d.%02d m/s",
                              sp < 0 ? '-' : '+',
                              (int)(sa / 1000), (int)((sa % 1000) / 10));
        ui_label_set_fmt(s_veh.thr, "%+d %%", st->out_v * 100 / SCR_DRIVE_V_MAX);
        ui_label_set_fmt(s_veh.str, "%+d %%", st->out_w * 100 / SCR_DRIVE_W_MAX);
        ui_label_set_fmt(s_veh.batt, "%u %%", st->batt_pct);
        ui_label_set_fmt(s_veh.battv, "%u.%02u V", st->batt_mv / 1000, (st->batt_mv % 1000) / 10);
        ui_label_set_fmt(s_veh.odo, "%u.%03u m",
                              (unsigned)(st->odo_session_mm / 1000),
                              (unsigned)(st->odo_session_mm % 1000));
        if (st->fault_code) {
            ui_label_set_fmt(s_veh.fault, "0x%04X", st->fault_code);
            ui_label_set_color(s_veh.fault, lv_color_hex(UI_COL_CRIT));
        } else {
            ui_label_set_text(s_veh.fault, "NONE");
            ui_label_set_color(s_veh.fault, lv_color_hex(UI_COL_OK));
        }
        ui_label_set_fmt(s_veh.tcfw, "v%u.%u.%u",
                              (unsigned)((st->tc_fw_ver >> 16) & 0xFF),
                              (unsigned)((st->tc_fw_ver >> 8) & 0xFF),
                              (unsigned)(st->tc_fw_ver & 0xFF));
        ui_label_set_fmt(s_veh.hw, "%u", st->hw_rev);
    } else {
        lv_obj_t *names[] = { s_veh.state, s_veh.speed, s_veh.thr, s_veh.str,
                              s_veh.batt, s_veh.battv, s_veh.odo, s_veh.fault,
                              s_veh.tcfw, s_veh.hw };
        for (int i = 0; i < 10; i++) {
            ui_label_set_text(names[i], "--");
            ui_label_set_color(names[i], lv_color_hex(UI_COL_DIM));
        }
    }
    scr_settings_t set;
    scr_settings_get(&set);
    static const char * const mode_txt[3] = { "ECO", "NORMAL", "SPORT" };
    ui_label_set_text(s_veh.mode, mode_txt[set.mode <= SCR_MODE_SPORT ? set.mode : 1]);
}

/* ---- Radio page (spec 24-28) -------------------------------------------------------*/

typedef struct {
    lv_obj_t *status, *rssi, *bar, *qual, *lat, *loss, *tx, *rx, *seq, *ch, *peer, *linkrtt;
} radio_t;

static radio_t s_radio;

void ui_pages_create_radio(lv_obj_t *root)
{
    ui_header(root, "RADIO", nav_home_cb);
    lv_obj_t *body = kv_body(root);

    s_radio.status = lv_label_create(body);
    ui_label_set_text(s_radio.status, "- SEARCHING");
    lv_obj_set_style_text_font(s_radio.status, F_LG, 0);
    ui_label_set_color(s_radio.status, lv_color_hex(UI_COL_DIM));

    /* RSSI with graphical + numeric display (spec 25) */
    lv_obj_t *row = ui_kv_row(body, "RSSI", &s_radio.rssi);
    s_radio.bar = lv_bar_create(row);
    lv_obj_set_size(s_radio.bar, 120, 10);
    lv_bar_set_range(s_radio.bar, 0, 100);
    lv_obj_align_to(s_radio.bar, row, LV_ALIGN_RIGHT_MID, -150, 0);
    lv_obj_set_style_bg_color(s_radio.bar, lv_color_hex(UI_COL_SURFACE2), 0);
    lv_obj_set_style_bg_opa(s_radio.bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_radio.bar, lv_color_hex(UI_COL_OK), LV_PART_INDICATOR);

    ui_kv_row(body, "Quality", &s_radio.qual);
    ui_kv_row(body, "Latency (S3-C6)", &s_radio.lat);
    ui_kv_row(body, "Loss", &s_radio.loss);
    ui_kv_row(body, "TX rate", &s_radio.tx);
    ui_kv_row(body, "RX rate", &s_radio.rx);
    kv_section(body, "SESSION");
    ui_kv_row(body, "Telemetry seq", &s_radio.seq);
    ui_kv_row(body, "Channel", &s_radio.ch);
    ui_kv_row(body, "Peer (C6 fw)", &s_radio.peer);
    ui_kv_row(body, "C6-TC275 RTT", &s_radio.linkrtt);
}

void ui_pages_radio_refresh(const scr_state_t *st)
{
    const char *s;
    lv_color_t c;
    if (st->conn == SCR_CONN_CONNECTED) {
        s = st->tele_fresh ? "CONNECTED" : "STALE";
        c = lv_color_hex(st->tele_fresh ? UI_COL_OK : UI_COL_WARN);
    } else if (st->conn == SCR_CONN_CONNECTING) {
        s = "CONNECTING"; c = lv_color_hex(UI_COL_INFO);
    } else {
        s = "SEARCHING"; c = lv_color_hex(UI_COL_DIM);
    }
    ui_label_set_fmt(s_radio.status, "- %s", s);
    ui_label_set_color(s_radio.status, c);

    if (st->rssi != 0) {
        ui_label_set_fmt(s_radio.rssi, "%d dBm", st->rssi);
        /* map -100..-40 dBm to 0..100 % */
        int pct = (st->rssi + 100) * 100 / 60;
        lv_bar_set_value(s_radio.bar, pct, LV_ANIM_OFF);
        lv_color_t bc = ui_col_for_state(st->quality >= SCR_QUAL_GOOD,
                                         st->quality == SCR_QUAL_FAIR ||
                                         st->quality == SCR_QUAL_WEAK,
                                         st->quality == SCR_QUAL_CRITICAL);
        lv_obj_set_style_bg_color(s_radio.bar, bc, LV_PART_INDICATOR);
        static const char * const qual_txt[6] = { "-", "EXCELLENT", "GOOD", "FAIR", "WEAK", "CRITICAL" };
        ui_label_set_text(s_radio.qual, qual_txt[st->quality]);
    } else {
        ui_label_set_text(s_radio.rssi, "--");
        lv_bar_set_value(s_radio.bar, 0, LV_ANIM_OFF);
        ui_label_set_text(s_radio.qual, "--");
    }

    ui_label_set_fmt(s_radio.lat, "%u ms (min %u / max %u)",
                          st->lat_ms, st->lat_min, st->lat_max);
    ui_label_set_fmt(s_radio.loss, "%u.%u %%", st->loss_pct_x10 / 10, st->loss_pct_x10 % 10);
    ui_label_set_fmt(s_radio.tx, "%u Hz", st->tx_rate);
    ui_label_set_fmt(s_radio.rx, "%u Hz", st->rx_rate);
    ui_label_set_fmt(s_radio.seq, "%u", (unsigned)st->tele_seq);
    ui_label_set_fmt(s_radio.ch, "%u", st->channel);
    ui_label_set_text(s_radio.peer, st->c6_fw);
    ui_label_set_fmt(s_radio.linkrtt, "%u ms (err %u.%u%%)",
                          st->link_rtt_ms, st->link_err_rate / 10, st->link_err_rate % 10);
}

/* ---- Diagnostics page (spec 29, engineer mode) ----------------------------------------*/

typedef struct {
    lv_obj_t *fw, *heap, *psram, *up;
    lv_obj_t *rssi, *loss, *lat;
    lv_obj_t *vlink, *vstate, *c6fw;
} diag_t;

static diag_t s_diag;

static void diag_events_cb(lv_event_t *e)
{
    ui_nav_open(UI_PAGE_EVENTS);
}

void ui_pages_create_diag(lv_obj_t *root)
{
    ui_header(root, "DIAGNOSTICS", nav_home_cb);
    lv_obj_t *body = kv_body(root);

    kv_section(body, "CONTROLLER (ESP32-S3)");
    ui_kv_row(body, "Firmware", &s_diag.fw);
    ui_kv_row(body, "Heap free", &s_diag.heap);
    ui_kv_row(body, "PSRAM free", &s_diag.psram);
    ui_kv_row(body, "Uptime", &s_diag.up);

    kv_section(body, "RADIO");
    ui_kv_row(body, "RSSI", &s_diag.rssi);
    ui_kv_row(body, "Loss", &s_diag.loss);
    ui_kv_row(body, "Latency min/max", &s_diag.lat);

    kv_section(body, "VEHICLE CHAIN");
    ui_kv_row(body, "C6 link", &s_diag.vlink);
    ui_kv_row(body, "TC275 state", &s_diag.vstate);
    ui_kv_row(body, "C6 firmware", &s_diag.c6fw);

    lv_obj_t *btn = ui_button(body, "EVENT LOG", lv_color_hex(UI_COL_SURFACE2), diag_events_cb, NULL);
    lv_obj_set_size(btn, LV_PCT(100), 44);
}

void ui_pages_diag_refresh(const scr_state_t *st)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ui_label_set_fmt(s_diag.fw, "v%s", app->version);
    ui_label_set_fmt(s_diag.heap, "%u KB",
                          (unsigned)(esp_get_free_heap_size() / 1024));
    ui_label_set_fmt(s_diag.psram, "%u KB",
                          (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    ui_label_set_fmt(s_diag.up, "%uh %02um",
                          (unsigned)(st->uptime_ms / 3600000),
                          (unsigned)((st->uptime_ms / 60000) % 60));

    ui_label_set_fmt(s_diag.rssi, "%d dBm", st->rssi);
    ui_label_set_fmt(s_diag.loss, "%u.%u %%", st->loss_pct_x10 / 10, st->loss_pct_x10 % 10);
    ui_label_set_fmt(s_diag.lat, "%u / %u ms", st->lat_min, st->lat_max);

    ui_label_set_text(s_diag.vlink, st->tc_on ? "OK" : "DOWN");
    lv_obj_set_style_text_color(s_diag.vlink,
                                lv_color_hex(st->tc_on ? UI_COL_OK : UI_COL_CRIT), 0);
    if (st->tele_fresh) {
        ui_label_set_fmt(s_diag.vstate, "0x%02X", st->veh_state);
    } else {
        ui_label_set_text(s_diag.vstate, "--");
    }
    ui_label_set_text(s_diag.c6fw, st->c6_fw);
}

/* ---- Pairing page (spec 39-41) -----------------------------------------------------------*/

typedef struct {
    lv_obj_t *status;
    lv_obj_t *steps;
    lv_obj_t *btn;
} pair_t;

static pair_t s_pair;

static void pair_cb(lv_event_t *e)
{
    scr_link_request_pair();
}

void ui_pages_create_pair(lv_obj_t *root)
{
    ui_header(root, "PAIRING", nav_home_cb);
    lv_obj_t *body = kv_body(root);

    s_pair.steps = lv_label_create(body);
    ui_label_set_text(s_pair.steps,
        "1. Power on the vehicle and stay in range\n"
        "2. Press the pairing button on the car for 3 s\n"
        "3. Touch PAIR below within the open window");
    ui_label_set_color(s_pair.steps, lv_color_hex(UI_COL_DIM));
    lv_obj_set_style_text_font(s_pair.steps, F_MD, 0);
    lv_label_set_long_mode(s_pair.steps, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_pair.steps, LV_PCT(100));

    s_pair.btn = ui_button(body, "PAIR WITH VEHICLE", lv_color_hex(UI_COL_ACCENT), pair_cb, NULL);
    lv_obj_set_size(s_pair.btn, LV_PCT(100), 52);

    s_pair.status = lv_label_create(body);
    ui_label_set_text(s_pair.status, " ");
    lv_obj_set_style_text_font(s_pair.status, F_MD, 0);
    lv_obj_set_width(s_pair.status, LV_PCT(100));
    lv_label_set_long_mode(s_pair.status, LV_LABEL_LONG_WRAP);
}

void ui_pages_pair_refresh(const scr_state_t *st)
{
    bool paired = false;
    scr_settings_t set;
    scr_settings_get(&set);
    paired = set.token[0] != '\0';

    ui_label_set_text(s_pair.status, st->pair_status[0] ? st->pair_status
                          : (paired ? "Paired. Token stored." : "Not paired."));
    lv_obj_set_style_text_color(s_pair.status,
        paired ? lv_color_hex(UI_COL_OK) : lv_color_hex(UI_COL_DIM), 0);
}

/* ---- Event history page (spec 83) -----------------------------------------------------------*/

static lv_obj_t *s_ev_body;
/* fingerprint of the newest rendered event: the ring wraps, so a plain count
 * stops changing once it is full and the page would freeze (spec 83) */
static bool     s_ev_seen;
static uint32_t s_ev_newest_ts;
static char     s_ev_newest_txt[SCR_EVENT_TEXT_MAX];

void ui_pages_create_events(lv_obj_t *root)
{
    ui_header(root, "EVENT LOG", nav_home_cb);
    s_ev_body = kv_body(root);
    lv_obj_set_style_pad_all(s_ev_body, 8, 0);
}

void ui_pages_events_refresh(const scr_state_t *st)
{
    scr_event_t evs[SCR_EVENT_RING_LEN];
    int n = app_state_events_get(evs, SCR_EVENT_RING_LEN);

    if (n == 0) {
        if (!s_ev_seen) {
            return;
        }
        s_ev_seen = false;
    } else {
        bool same = s_ev_seen && evs[0].ts_ms == s_ev_newest_ts &&
                    strncmp(evs[0].text, s_ev_newest_txt,
                            sizeof(s_ev_newest_txt)) == 0;
        if (same) {
            return;
        }
        s_ev_seen = true;
        s_ev_newest_ts = evs[0].ts_ms;
        snprintf(s_ev_newest_txt, sizeof(s_ev_newest_txt), "%s", evs[0].text);
    }

    lv_obj_clean(s_ev_body);
    for (int i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_create(s_ev_body);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_ver(row, 2, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *l = lv_label_create(row);
        ui_label_set_fmt(l, "%02u:%02u:%02u  %s",
                              (unsigned)(evs[i].ts_ms / 3600000),
                              (unsigned)((evs[i].ts_ms / 60000) % 60),
                              (unsigned)((evs[i].ts_ms / 1000) % 60),
                              evs[i].text);
        lv_color_t col = lv_color_hex(UI_COL_TXT);
        if (evs[i].level == SCR_LOG_CRIT)      col = lv_color_hex(UI_COL_CRIT);
        else if (evs[i].level == SCR_LOG_WARN) col = lv_color_hex(UI_COL_WARN);
        else if (evs[i].level == SCR_LOG_NOTICE) col = lv_color_hex(UI_COL_INFO);
        else                                   col = lv_color_hex(UI_COL_DIM);
        ui_label_set_color(l, col);
        lv_obj_set_style_text_font(l, F_SM, 0);
    }
}

/* ---- Settings page (spec 31-38) ------------------------------------------------------------------*/

typedef struct {
    lv_obj_t *root;
    lv_obj_t *list;
    lv_obj_t *subs[5];
    sub_t cur;
    /* control sub */
    lv_obj_t *mode_btns[3];
    lv_obj_t *dz_lbl;
    lv_obj_t *dz_slider;
    /* radio sub */
    lv_obj_t *ssid_ta, *pass_ta, *kb;
    lv_timer_t *kb_hide;        /* delayed keyboard hide (defocus race) */
    lv_obj_t *pair_status_lbl;
    /* command deck (settings root) */
    lv_obj_t *core_arc;         /* radial link-quality gauge          */
    lv_obj_t *core_val;         /* dBm in the gauge centre            */
    lv_obj_t *core_q;           /* quality word under the number      */
    lv_obj_t *chip[3];          /* LINK / CAR / CTRL status chips     */
    lv_obj_t *foot;             /* uptime + firmware line             */
    lv_obj_t *tile_sub[SUB_NAV_EVENTS + 1];  /* live subtitle per module */
    /* about sub */
    lv_obj_t *fw_val;
    int fw_taps;
    int64_t fw_first_tap;
} set_t;

static set_t s_set;

static void set_open_sub(sub_t s);

static void sub_back_cb(lv_event_t *e)
{
    set_open_sub(SUB_NONE);
}

static void list_row_cb(lv_event_t *e)
{
    sub_t s = (sub_t)(uintptr_t)lv_event_get_user_data(e);
    if (s == SUB_NAV_PAIR) {
        ui_nav_open(UI_PAGE_PAIR);
        return;
    }
    if (s == SUB_NAV_DIAG) {
        ui_nav_open(UI_PAGE_DIAG);
        return;
    }
    if (s == SUB_NAV_EVENTS) {
        ui_nav_open(UI_PAGE_EVENTS);
        return;
    }
    set_open_sub(s);
}

/* mode / deadzone handlers */
static void mode_btn_cb(lv_event_t *e)
{
    int m = (int)(uintptr_t)lv_event_get_user_data(e);
    scr_settings_t set;
    scr_settings_get(&set);
    scr_settings_set_control((uint8_t)m, set.deadzone_pct);
    app_state_set_mode((scr_mode_t)m);
    for (int i = 0; i < 3; i++) {
        if (i == m) {
            lv_obj_add_state(s_set.mode_btns[i], LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(s_set.mode_btns[i], LV_STATE_CHECKED);
        }
    }
    /* spec 34: mode switch must show feedback */
    ui_toast("%s MODE", m == 0 ? "ECO" : m == 1 ? "NORMAL" : "SPORT");
}

static void dz_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    ui_label_set_fmt(s_set.dz_lbl, "Dead zone: %d %%", v);
}

static void dz_released_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    scr_settings_t set;
    scr_settings_get(&set);
    scr_settings_set_control(set.mode, (uint8_t)lv_slider_get_value(slider));
}

/* radio sub handlers */
static void ta_focus_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (s_set.kb_hide) {                    /* refocusing: cancel the hide */
        lv_timer_del(s_set.kb_hide);
        s_set.kb_hide = NULL;
    }
    if (s_set.kb) {
        lv_keyboard_set_textarea(s_set.kb, ta);
        lv_obj_remove_flag(s_set.kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void kb_hide_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_set.kb_hide = NULL;
    if (s_set.kb) {
        lv_obj_add_flag(s_set.kb, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ta_defocus_cb(lv_event_t *e)
{
    (void)e;
    /* delayed hide: DEFOCUSED(old)/FOCUSED(new) ordering between our two
     * textareas must never leave the keyboard hidden while a textarea has
     * focus; if nothing refocused within 50 ms it was a real dismissal */
    if (s_set.kb_hide) {
        lv_timer_del(s_set.kb_hide);
    }
    s_set.kb_hide = lv_timer_create(kb_hide_timer_cb, 50, NULL);
}

static void wifi_apply_cb(lv_event_t *e)
{
    scr_settings_t set;
    scr_settings_get(&set);
    strlcpy(set.ssid, lv_textarea_get_text(s_set.ssid_ta), sizeof(set.ssid));
    strlcpy(set.pass, lv_textarea_get_text(s_set.pass_ta), sizeof(set.pass));
    scr_settings_update(&set);
    scr_link_apply_wifi();
    ui_toast("Wi-Fi applied, reconnecting");
}

static void pair_btn_cb(lv_event_t *e)
{
    scr_link_request_pair();
}

/* about sub: engineer mode easter egg (spec 87): 7 taps on the FW row */
static void fw_tap_cb(lv_event_t *e)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_set.fw_first_tap > 3000) {
        s_set.fw_taps = 0;
        s_set.fw_first_tap = now;
    }
    if (++s_set.fw_taps >= 7) {
        s_set.fw_taps = 0;
        ui_engineer_enable();
    }
}

/* ---- build ---------------------------------------------------------------------------------------*/

static lv_obj_t *make_sub(lv_obj_t *root)
{
    lv_obj_t *p = lv_obj_create(root);
    lv_obj_set_size(p, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(p, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(p);
    return p;
}

/* ---- command deck --------------------------------------------------------
 * The settings root is not a list: it is a console.  Left: a live SYSTEM CORE
 * gauge (link quality ring + status chips).  Right: numbered module tiles, each
 * showing its current value, so the page answers "what is set right now"
 * before anything is tapped.  All decoration is static outlines - the only
 * things that repaint are values that actually change (cached setters). */

/* HUD corner bracket: two thin bars forming an L at one corner of `parent` */
static void hud_bracket(lv_obj_t *parent, lv_align_t al, int dx, int dy)
{
    static const int L = 14, T = 2;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = lv_obj_create(parent);
        lv_obj_set_size(b, i ? T : L, i ? L : T);
        lv_obj_set_style_bg_color(b, lv_color_hex(UI_COL_ACCENT), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_set_style_radius(b, 0, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_align(b, al, dx, dy);
    }
}

static lv_obj_t *status_chip(lv_obj_t *parent, const char *txt)
{
    lv_obj_t *c = lv_label_create(parent);
    ui_label_set_text(c, txt);
    lv_obj_set_style_text_font(c, F_SM, 0);
    lv_obj_set_style_text_letter_space(c, 1, 0);
    lv_obj_set_style_pad_hor(c, 8, 0);
    lv_obj_set_style_pad_ver(c, 3, 0);
    lv_obj_set_style_radius(c, 4, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(UI_COL_LINE), 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(UI_COL_SURFACE2), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    return c;
}

/* numbered module tile: 01 index, icon, title, live subtitle, accent strip */
static void module_tile(lv_obj_t *grid, int idx, const char *icon,
                        const char *title, sub_t target)
{
    lv_obj_t *t = lv_button_create(grid);
    lv_obj_set_size(t, LV_PCT(48), 88);
    lv_obj_set_style_bg_color(t, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(UI_COL_SURFACE2), LV_STATE_PRESSED);
    lv_obj_set_style_radius(t, 6, 0);
    lv_obj_set_style_shadow_width(t, 0, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(UI_COL_LINE), 0);
    lv_obj_set_style_border_color(t, lv_color_hex(UI_COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_set_style_pad_all(t, 0, 0);
    lv_obj_add_event_cb(t, list_row_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)target);

    lv_obj_t *strip = lv_obj_create(t);
    lv_obj_set_size(strip, 3, 56);
    lv_obj_set_style_bg_color(strip, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(strip, 0, 0);
    lv_obj_set_style_radius(strip, 0, 0);
    lv_obj_remove_flag(strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(strip, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *n = lv_label_create(t);
    lv_label_set_text_fmt(n, "%02d", idx);
    lv_obj_set_style_text_font(n, F_SM, 0);
    lv_obj_set_style_text_color(n, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_text_letter_space(n, 2, 0);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 14, 10);

    lv_obj_t *ic = lv_label_create(t);
    lv_label_set_text(ic, icon);
    lv_obj_set_style_text_font(ic, F_XL, 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(UI_COL_LINE), 0);
    lv_obj_set_style_text_color(ic, lv_color_hex(UI_COL_ACCENT), LV_STATE_PRESSED);
    lv_obj_align(ic, LV_ALIGN_RIGHT_MID, -14, 0);

    lv_obj_t *tt = lv_label_create(t);
    lv_label_set_text(tt, title);
    lv_obj_set_style_text_font(tt, F_LG, 0);
    lv_obj_set_style_text_color(tt, lv_color_hex(UI_COL_TXT), 0);
    lv_obj_set_style_text_letter_space(tt, 2, 0);
    lv_obj_align(tt, LV_ALIGN_LEFT_MID, 14, 2);

    lv_obj_t *sub = lv_label_create(t);
    lv_label_set_text(sub, " ");
    lv_obj_set_width(sub, LV_PCT(70));
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(sub, F_SM, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 14, -10);
    s_set.tile_sub[target] = sub;

    hud_bracket(t, LV_ALIGN_TOP_RIGHT, 0, 0);
}

void ui_pages_create_settings(lv_obj_t *root)
{
    memset(&s_set, 0, sizeof(s_set));
    s_set.root = root;
    s_set.cur = SUB_NONE;

    /* command deck: header (back to Home - the root must never be a dead
     * end) + SYSTEM CORE panel + module grid.  Everything lives inside
     * s_set.list so it hides as one when a sub-page opens. */
    int W = bsp_display_get_h_res(), H = bsp_display_get_v_res();
    s_set.list = lv_obj_create(root);
    lv_obj_set_size(s_set.list, W, H);
    lv_obj_align(s_set.list, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(s_set.list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_set.list, 0, 0);
    lv_obj_set_style_pad_all(s_set.list, 0, 0);
    lv_obj_remove_flag(s_set.list, LV_OBJ_FLAG_SCROLLABLE);

    ui_header(s_set.list, "COMMAND DECK", nav_home_cb);

    /* ---- SYSTEM CORE (left) ---- */
    int core_w = 262;
    lv_obj_t *core = ui_card(s_set.list);
    lv_obj_set_size(core, core_w, H - 58 - 8);
    lv_obj_align(core, LV_ALIGN_TOP_LEFT, 8, 58);
    lv_obj_set_style_pad_all(core, 0, 0);
    hud_bracket(core, LV_ALIGN_TOP_LEFT, 0, 0);
    hud_bracket(core, LV_ALIGN_BOTTOM_RIGHT, 0, 0);

    lv_obj_t *ct = lv_label_create(core);
    lv_label_set_text(ct, "SYSTEM CORE");
    lv_obj_set_style_text_font(ct, F_SM, 0);
    lv_obj_set_style_text_color(ct, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_text_letter_space(ct, 4, 0);
    lv_obj_align(ct, LV_ALIGN_TOP_MID, 0, 12);

    s_set.core_arc = lv_arc_create(core);
    lv_obj_set_size(s_set.core_arc, 184, 184);
    lv_arc_set_bg_angles(s_set.core_arc, 135, 45);     /* 270 deg sweep */
    lv_arc_set_range(s_set.core_arc, 0, 100);
    lv_arc_set_value(s_set.core_arc, 0);
    lv_obj_remove_style(s_set.core_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_set.core_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_set.core_arc, 10, 0);
    lv_obj_set_style_arc_color(s_set.core_arc, lv_color_hex(UI_COL_SURFACE2), 0);
    lv_obj_set_style_arc_width(s_set.core_arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_set.core_arc, lv_color_hex(UI_COL_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_set.core_arc, false, LV_PART_INDICATOR);
    lv_obj_align(s_set.core_arc, LV_ALIGN_TOP_MID, 0, 38);

    /* inner hairline ring: static depth cue */
    lv_obj_t *inner = lv_obj_create(core);
    lv_obj_set_size(inner, 136, 136);
    lv_obj_set_style_radius(inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(inner, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(inner, 1, 0);
    lv_obj_set_style_border_color(inner, lv_color_hex(UI_COL_LINE), 0);
    lv_obj_remove_flag(inner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align_to(inner, s_set.core_arc, LV_ALIGN_CENTER, 0, 0);

    s_set.core_val = lv_label_create(core);
    lv_label_set_text(s_set.core_val, "--");
    lv_obj_set_style_text_font(s_set.core_val, F_XL, 0);
    lv_obj_align_to(s_set.core_val, s_set.core_arc, LV_ALIGN_CENTER, 0, -8);
    lv_obj_t *unit = lv_label_create(core);
    lv_label_set_text(unit, "dBm");
    lv_obj_set_style_text_font(unit, F_SM, 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_align_to(unit, s_set.core_arc, LV_ALIGN_CENTER, 0, 18);
    s_set.core_q = lv_label_create(core);
    lv_label_set_text(s_set.core_q, "NO SIGNAL");
    lv_obj_set_style_text_font(s_set.core_q, F_SM, 0);
    lv_obj_set_style_text_letter_space(s_set.core_q, 2, 0);
    lv_obj_set_style_text_color(s_set.core_q, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_align_to(s_set.core_q, s_set.core_arc, LV_ALIGN_BOTTOM_MID, 0, 4);

    lv_obj_t *chips = lv_obj_create(core);
    lv_obj_set_size(chips, core_w - 16, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chips, 0, 0);
    lv_obj_set_style_pad_all(chips, 0, 0);
    lv_obj_set_style_pad_column(chips, 6, 0);
    lv_obj_remove_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_align(chips, LV_ALIGN_TOP_MID, 0, 262);
    s_set.chip[0] = status_chip(chips, "LINK --");
    s_set.chip[1] = status_chip(chips, "CAR --");
    s_set.chip[2] = status_chip(chips, "CTRL --");

    s_set.foot = lv_label_create(core);
    lv_label_set_text(s_set.foot, " ");
    lv_obj_set_width(s_set.foot, core_w - 20);
    lv_label_set_long_mode(s_set.foot, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_set.foot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_set.foot, F_SM, 0);
    lv_obj_set_style_text_color(s_set.foot, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_set_style_text_line_space(s_set.foot, 4, 0);
    lv_obj_align(s_set.foot, LV_ALIGN_BOTTOM_MID, 0, -14);

    /* ---- module grid (right) ---- */
    lv_obj_t *items = lv_obj_create(s_set.list);
    lv_obj_set_size(items, W - core_w - 24, H - 58 - 8);
    lv_obj_align(items, LV_ALIGN_TOP_RIGHT, -8, 58);
    lv_obj_set_style_bg_opa(items, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(items, 0, 0);
    lv_obj_set_style_pad_all(items, 0, 0);
    lv_obj_set_style_pad_row(items, 10, 0);
    lv_obj_set_flex_flow(items, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(items, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_scroll_dir(items, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(items, LV_SCROLLBAR_MODE_OFF);

    int idx = 1;
    module_tile(items, idx++, LV_SYMBOL_SHUFFLE, "CONTROL", SUB_CONTROL);
    module_tile(items, idx++, LV_SYMBOL_WIFI,    "RADIO",   SUB_RADIO);
    module_tile(items, idx++, LV_SYMBOL_EYE_OPEN, "DISPLAY", SUB_DISPLAY);
    module_tile(items, idx++, LV_SYMBOL_BLUETOOTH, "PAIRING", SUB_NAV_PAIR);
    module_tile(items, idx++, LV_SYMBOL_LIST,    "ABOUT",   SUB_ABOUT);
    if (ui_engineer_mode()) {
        module_tile(items, idx++, LV_SYMBOL_SETTINGS, "DIAG",   SUB_NAV_DIAG);
        module_tile(items, idx++, LV_SYMBOL_FILE,     "EVENTS", SUB_NAV_EVENTS);
    }
    /* static tile subtitles; live ones are written by the refresh */
    ui_label_set_text(s_set.tile_sub[SUB_DISPLAY], "Dark HUD  -  always on");
    ui_label_set_text(s_set.tile_sub[SUB_NAV_PAIR], "Bind this remote to a car");
    if (s_set.tile_sub[SUB_NAV_DIAG]) {
        ui_label_set_text(s_set.tile_sub[SUB_NAV_DIAG], "Link + vehicle internals");
        ui_label_set_text(s_set.tile_sub[SUB_NAV_EVENTS], "Event log");
    }

    /* ---- Control sub (spec 32/33) ---- */
    s_set.subs[SUB_CONTROL] = make_sub(root);
    lv_obj_t *h1 = ui_header(s_set.subs[SUB_CONTROL], "CONTROL", sub_back_cb);
    (void)h1;
    lv_obj_t *b1 = kv_body(s_set.subs[SUB_CONTROL]);

    lv_obj_t *mode_row = lv_obj_create(b1);
    lv_obj_set_size(mode_row, LV_PCT(100), 48);
    lv_obj_set_style_bg_opa(mode_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mode_row, 0, 0);
    lv_obj_remove_flag(mode_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *mode_lbl = lv_label_create(mode_row);
    ui_label_set_text(mode_lbl, "Mode");
    ui_label_set_color(mode_lbl, lv_color_hex(UI_COL_DIM));
    lv_obj_align(mode_lbl, LV_ALIGN_LEFT_MID, 10, 0);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *mb = lv_button_create(mode_row);
        lv_obj_set_size(mb, 84, 38);
        lv_obj_align(mb, LV_ALIGN_RIGHT_MID, -(2 - i) * 90 - 8, 0);
        lv_obj_set_style_bg_color(mb, lv_color_hex(UI_COL_SURFACE2), 0);
        /* selected mode turns accent: CHECKED state has no default look */
        lv_obj_set_style_bg_color(mb, lv_color_hex(UI_COL_ACCENT), LV_STATE_CHECKED);
        lv_obj_add_event_cb(mb, mode_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *ml = lv_label_create(mb);
        ui_label_set_text(ml, i == 0 ? "ECO" : i == 1 ? "NORMAL" : "SPORT");
        lv_obj_set_style_text_font(ml, F_SM, 0);
        lv_obj_center(ml);
        s_set.mode_btns[i] = mb;
    }

    lv_obj_t *dz_row = lv_obj_create(b1);
    lv_obj_set_size(dz_row, LV_PCT(100), 44);
    lv_obj_set_style_bg_opa(dz_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dz_row, 0, 0);
    lv_obj_remove_flag(dz_row, LV_OBJ_FLAG_SCROLLABLE);
    s_set.dz_lbl = lv_label_create(dz_row);
    lv_obj_align(s_set.dz_lbl, LV_ALIGN_LEFT_MID, 10, 0);
    ui_label_set_color(s_set.dz_lbl, lv_color_hex(UI_COL_DIM));
    lv_obj_t *dz_sl = lv_slider_create(dz_row);
    s_set.dz_slider = dz_sl;
    lv_obj_set_width(dz_sl, 200);
    lv_obj_align(dz_sl, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_slider_set_range(dz_sl, 0, 30);
    lv_obj_add_event_cb(dz_sl, dz_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(dz_sl, dz_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_t *note = lv_label_create(b1);
    ui_label_set_text(note, "Dead zone avoids crawling from finger jitter (spec 15).\nMode scales the joystick output on the remote.");
    ui_label_set_color(note, lv_color_hex(UI_COL_DIM));
    lv_obj_set_style_text_font(note, F_SM, 0);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);

    /* ---- Radio sub (spec 35) ---- */
    s_set.subs[SUB_RADIO] = make_sub(root);
    ui_header(s_set.subs[SUB_RADIO], "RADIO SETUP", sub_back_cb);
    lv_obj_t *b2 = kv_body(s_set.subs[SUB_RADIO]);

    lv_obj_t *ssid_lbl = lv_label_create(b2);
    ui_label_set_text(ssid_lbl, "C6 AP SSID");
    ui_label_set_color(ssid_lbl, lv_color_hex(UI_COL_DIM));
    s_set.ssid_ta = lv_textarea_create(b2);
    lv_obj_set_size(s_set.ssid_ta, LV_PCT(100), 44);
    lv_textarea_set_one_line(s_set.ssid_ta, true);
    lv_obj_add_event_cb(s_set.ssid_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_set.ssid_ta, ta_defocus_cb, LV_EVENT_DEFOCUSED, NULL);

    lv_obj_t *pass_lbl = lv_label_create(b2);
    ui_label_set_text(pass_lbl, "Password");
    ui_label_set_color(pass_lbl, lv_color_hex(UI_COL_DIM));
    s_set.pass_ta = lv_textarea_create(b2);
    lv_obj_set_size(s_set.pass_ta, LV_PCT(100), 44);
    lv_textarea_set_one_line(s_set.pass_ta, true);
    lv_textarea_set_password_mode(s_set.pass_ta, true);
    lv_obj_add_event_cb(s_set.pass_ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(s_set.pass_ta, ta_defocus_cb, LV_EVENT_DEFOCUSED, NULL);

    lv_obj_t *apply = ui_button(b2, "APPLY + RECONNECT", lv_color_hex(UI_COL_ACCENT), wifi_apply_cb, NULL);
    lv_obj_set_size(apply, LV_PCT(100), 48);

    lv_obj_t *pairb = ui_button(b2, "PAIR WITH VEHICLE", lv_color_hex(UI_COL_SURFACE2), pair_btn_cb, NULL);
    lv_obj_set_size(pairb, LV_PCT(100), 48);

    s_set.pair_status_lbl = lv_label_create(b2);
    lv_obj_set_width(s_set.pair_status_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_set.pair_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_set.pair_status_lbl, F_SM, 0);

    s_set.kb = lv_keyboard_create(s_set.subs[SUB_RADIO]);
    lv_obj_set_size(s_set.kb, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(s_set.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_set.kb, LV_OBJ_FLAG_HIDDEN);

    /* ---- Display sub (spec 36-38) ---- */
    s_set.subs[SUB_DISPLAY] = make_sub(root);
    ui_header(s_set.subs[SUB_DISPLAY], "DISPLAY", sub_back_cb);
    lv_obj_t *b3 = kv_body(s_set.subs[SUB_DISPLAY]);
    ui_kv_row(b3, "Theme", NULL);
    lv_obj_t *tv = lv_obj_get_child(lv_obj_get_child(b3, -1), 1);
    ui_label_set_text(tv, "DARK (default)");
    ui_kv_row(b3, "Brightness", NULL);
    lv_obj_t *bv = lv_obj_get_child(lv_obj_get_child(b3, -1), 1);
    ui_label_set_text(bv, "fixed on this panel");
    lv_obj_t *dnote = lv_label_create(b3);
    ui_label_set_text(dnote, "The SUB3 panel backlight is hard-wired on the\nEV board; brightness control is not available.\nDriving mode keeps the screen always on (spec 37).");
    ui_label_set_color(dnote, lv_color_hex(UI_COL_DIM));
    lv_obj_set_style_text_font(dnote, F_SM, 0);
    lv_obj_set_width(dnote, LV_PCT(100));
    lv_label_set_long_mode(dnote, LV_LABEL_LONG_WRAP);

    /* ---- About sub (spec 45 + engineer trigger) ---- */
    s_set.subs[SUB_ABOUT] = make_sub(root);
    ui_header(s_set.subs[SUB_ABOUT], "ABOUT", sub_back_cb);
    lv_obj_t *b4 = kv_body(s_set.subs[SUB_ABOUT]);

    lv_obj_t *prod = lv_label_create(b4);
    ui_label_set_text(prod, "SMART CAR REMOTE");
    lv_obj_set_style_text_font(prod, F_LG, 0);
    ui_kv_row(b4, "Controller", NULL);
    lv_obj_t *cv = lv_obj_get_child(lv_obj_get_child(b4, -1), 1);
    ui_label_set_text(cv, "ESP32-S3");
    ui_kv_row(b4, "Radio", NULL);
    lv_obj_t *rv = lv_obj_get_child(lv_obj_get_child(b4, -1), 1);
    ui_label_set_text(rv, "WiFi -> C6, proto v2");
    ui_kv_row(b4, "Vehicle", NULL);
    lv_obj_t *vv = lv_obj_get_child(lv_obj_get_child(b4, -1), 1);
    ui_label_set_text(vv, "TC275 via C6");
    ui_kv_row(b4, "Firmware", &s_set.fw_val);
    const esp_app_desc_t *app = esp_app_get_description();
    ui_label_set_fmt(s_set.fw_val, "v%s", app->version);
    lv_obj_add_flag(s_set.fw_val, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_set.fw_val, fw_tap_cb, LV_EVENT_CLICKED, NULL);
}

/* pairing and diagnostics live in their own pages; map the list rows to nav */
void ui_pages_create(lv_obj_t *vehicle, lv_obj_t *radio, lv_obj_t *diag,
                     lv_obj_t *settings, lv_obj_t *pair, lv_obj_t *events)
{
    ui_pages_create_vehicle(vehicle);
    ui_pages_create_radio(radio);
    ui_pages_create_diag(diag);
    ui_pages_create_settings(settings);
    ui_pages_create_pair(pair);
    ui_pages_create_events(events);
}

void ui_pages_rebuild(void)
{
    /* settings list gains engineer entries; easiest correct rebuild:
     * wipe the whole settings page and build it again */
    if (s_set.root) {
        lv_obj_clean(s_set.root);
        ui_pages_create_settings(s_set.root);
    }
}

/* one-shot value population on sub-page open; the 10 Hz refresh must NOT
 * rewrite textareas/sliders the user is interacting with */
static void sub_sync_values(void)
{
    scr_settings_t set;
    scr_settings_get(&set);
    if (s_set.cur == SUB_CONTROL) {
        for (int i = 0; i < 3; i++) {
            if ((uint8_t)i == set.mode) {
                lv_obj_add_state(s_set.mode_btns[i], LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(s_set.mode_btns[i], LV_STATE_CHECKED);
            }
        }
        ui_label_set_fmt(s_set.dz_lbl, "Dead zone: %d %%", set.deadzone_pct);
        if (s_set.dz_slider) {
            lv_slider_set_value(s_set.dz_slider, set.deadzone_pct, LV_ANIM_OFF);
        }
    } else if (s_set.cur == SUB_RADIO) {
        lv_textarea_set_text(s_set.ssid_ta, set.ssid);
        lv_textarea_set_text(s_set.pass_ta, set.pass);
    }
}

static void set_open_sub(sub_t s)
{
    if (s == SUB_NONE) {
        lv_obj_add_flag(s_set.subs[s_set.cur], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_set.list, LV_OBJ_FLAG_HIDDEN);
        s_set.cur = SUB_NONE;
        return;
    }
    if (s_set.cur != SUB_NONE) {
        lv_obj_add_flag(s_set.subs[s_set.cur], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_set.list, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_remove_flag(s_set.subs[s], LV_OBJ_FLAG_HIDDEN);
    s_set.cur = s;
    sub_sync_values();
}

static void chip_set(lv_obj_t *c, const char *txt, lv_color_t col)
{
    ui_label_set_text(c, txt);
    ui_label_set_color(c, col);
    if (!lv_color_eq(lv_obj_get_style_border_color(c, 0), col)) {
        lv_obj_set_style_border_color(c, col, 0);
    }
}

static void deck_refresh(const scr_state_t *st)
{
    /* SYSTEM CORE: RSSI -> 0..100 % (-90 dBm = 0, -40 dBm = 100) */
    static const char * const qtxt[6] = { "NO SIGNAL", "EXCELLENT", "GOOD",
                                          "FAIR", "WEAK", "CRITICAL" };
    lv_color_t qc;
    if (st->rssi != 0) {
        int pct = (st->rssi + 90) * 2;
        pct = pct < 0 ? 0 : (pct > 100 ? 100 : pct);
        if (lv_arc_get_value(s_set.core_arc) != pct) {
            lv_arc_set_value(s_set.core_arc, pct);
        }
        qc = ui_col_for_state(st->quality >= SCR_QUAL_GOOD,
                              st->quality == SCR_QUAL_FAIR || st->quality == SCR_QUAL_WEAK,
                              st->quality == SCR_QUAL_CRITICAL);
        ui_label_set_fmt(s_set.core_val, "%d", st->rssi);
        ui_label_set_text(s_set.core_q, qtxt[st->quality <= 5 ? st->quality : 0]);
    } else {
        if (lv_arc_get_value(s_set.core_arc) != 0) {
            lv_arc_set_value(s_set.core_arc, 0);
        }
        qc = lv_color_hex(UI_COL_DIM);
        ui_label_set_text(s_set.core_val, "--");
        ui_label_set_text(s_set.core_q, qtxt[0]);
    }
    ui_label_set_color(s_set.core_q, qc);
    if (!lv_color_eq(lv_obj_get_style_arc_color(s_set.core_arc, LV_PART_INDICATOR), qc)) {
        lv_obj_set_style_arc_color(s_set.core_arc, qc, LV_PART_INDICATOR);
    }

    chip_set(s_set.chip[0], st->conn == SCR_CONN_CONNECTED ? "LINK ON" :
                            st->conn == SCR_CONN_CONNECTING ? "LINK ..." : "LINK OFF",
             st->conn == SCR_CONN_CONNECTED ? lv_color_hex(UI_COL_OK) :
             st->conn == SCR_CONN_CONNECTING ? lv_color_hex(UI_COL_WARN) : lv_color_hex(UI_COL_DIM));
    chip_set(s_set.chip[1], st->tc_on ? "CAR ON" : "CAR OFF",
             st->tc_on ? lv_color_hex(UI_COL_OK) : lv_color_hex(UI_COL_DIM));
    chip_set(s_set.chip[2], st->owner == SCR_OWNER_S3 ? "CTRL S3" :
                            st->owner == SCR_OWNER_WEB ? "CTRL WEB" : "CTRL --",
             st->owner == SCR_OWNER_S3 ? lv_color_hex(UI_COL_OK) :
             st->owner == SCR_OWNER_WEB ? lv_color_hex(UI_COL_INFO) : lv_color_hex(UI_COL_WARN));

    uint32_t up = st->uptime_ms / 1000u;
    const esp_app_desc_t *app = esp_app_get_description();
    ui_label_set_fmt(s_set.foot, "UP %02lu:%02lu:%02lu   CH %u\nS3 v%s   C6 %s",
                     (unsigned long)(up / 3600u), (unsigned long)(up / 60u % 60u),
                     (unsigned long)(up % 60u), st->channel, app->version,
                     st->c6_fw[0] ? st->c6_fw : "--");

    /* live module subtitles */
    scr_settings_t set;
    scr_settings_get(&set);
    static const char * const mtxt[3] = { "ECO", "NORMAL", "SPORT" };
    ui_label_set_fmt(s_set.tile_sub[SUB_CONTROL], "%s  -  dead zone %u%%",
                     mtxt[set.mode <= 2 ? set.mode : 1], set.deadzone_pct);
    ui_label_set_fmt(s_set.tile_sub[SUB_RADIO], "%s", set.ssid[0] ? set.ssid : "not set");
    ui_label_set_fmt(s_set.tile_sub[SUB_ABOUT], "Firmware v%s", app->version);
}

void ui_pages_settings_refresh(const scr_state_t *st)
{
    /* live values only; user-editable widgets are populated once on open
     * (sub_sync_values) so a 10 Hz repaint can never fight the keyboard */
    if (s_set.cur == SUB_NONE) {
        deck_refresh(st);
    }
    if (s_set.cur == SUB_RADIO) {
        ui_label_set_text(s_set.pair_status_lbl,
                          st->pair_status[0] ? st->pair_status : " ");
    }
}
