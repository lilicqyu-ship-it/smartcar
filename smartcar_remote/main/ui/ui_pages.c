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
#include "../scr_svc.h"
#include "../proto/proto_frames.h"
#include "../proto/vision.h"

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
    SUB_NAV_FW,     /* jumps to FIRMWARE (doc/08)  */
    SUB_NAV_CALIB,  /* jumps to CALIBRATE          */
    SUB_NAV_FDIAG,  /* jumps to DIAGNOSE           */
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
    lv_obj_t *camlink;
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
    /* Camera WS half-open diagnosis (design doc 8.4): silent link shows here
     * long before it matters on the CAMERA page */
    ui_kv_row(body, "CAM link", &s_radio.camlink);
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

    const char *cam_txt;
    lv_color_t cam_col;
    switch (st->cam.conn) {
        case SCR_CAM_CONNECTED:
            if (st->cam.stale)              { cam_txt = "UP / STALE";   cam_col = lv_color_hex(UI_COL_WARN); }
            else if (st->cam.subscribed)    { cam_txt = "UP / STREAM";  cam_col = lv_color_hex(UI_COL_OK); }
            else                            { cam_txt = "UP / IDLE";    cam_col = lv_color_hex(UI_COL_DIM); }
            break;
        case SCR_CAM_CONNECTING:            cam_txt = "CONNECTING";     cam_col = lv_color_hex(UI_COL_INFO); break;
        default:                            cam_txt = "OFF";            cam_col = lv_color_hex(UI_COL_DIM); break;
    }
    ui_label_set_text(s_radio.camlink, cam_txt);
    ui_label_set_color(s_radio.camlink, cam_col);
}

/* ---- Diagnostics page (spec 29, engineer mode) ----------------------------------------*/

typedef struct {
    lv_obj_t *fw, *heap, *psram, *up;
    lv_obj_t *rssi, *loss, *lat;
    lv_obj_t *vlink, *vstate, *c6fw;
    lv_obj_t *cam_link, *cam_stream, *cam_err, *vision;
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

    /* camera/vision plane (design doc 8.4): counters mirrored by scr_cam at
     * 1 Hz; drop/frame_err separate transport loss from bad frames */
    kv_section(body, "CAMERA / VISION");
    ui_kv_row(body, "Cam link", &s_diag.cam_link);
    ui_kv_row(body, "Cam stream", &s_diag.cam_stream);
    ui_kv_row(body, "Cam errors", &s_diag.cam_err);
    ui_kv_row(body, "Vision", &s_diag.vision);

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

    ui_label_set_fmt(s_diag.cam_link, "%s %s %ux%u",
                          st->cam.conn == SCR_CAM_CONNECTED ? "UP"
                              : st->cam.conn == SCR_CAM_CONNECTING ? "LINKING" : "OFF",
                          st->cam.sensor[0] ? st->cam.sensor : "-",
                          st->cam.w, st->cam.h);
    ui_label_set_fmt(s_diag.cam_stream, "%u.%u fps  seq %lu",
                          st->cam.fps_x10 / 10, st->cam.fps_x10 % 10,
                          (unsigned long)st->cam.seq);
    ui_label_set_fmt(s_diag.cam_err, "drop %lu  frame %lu  dec %lu (%ums) e2e %ums rtt %ums",
                          (unsigned long)st->cam.drop,
                          (unsigned long)st->cam.frame_err,
                          (unsigned long)st->cam.decode_err,
                          st->cam.decode_ms_max, st->cam.e2e_ms, st->cam.ping_rtt_ms);
    ui_label_set_fmt(s_diag.vision, "%s %s conf %u%% err %+d.%03d %s",
                          vision_mode_str(st->vision.mode < VISION_MODE_IDX_COUNT
                                              ? st->vision.mode : VISION_MODE_IDX_OFF),
                          st->vision.valid ? "valid" : "n/a",
                          st->vision.confidence,
                          st->vision.error_x1000 / 1000,
                          (int)(st->vision.error_x1000 < 0 ? -st->vision.error_x1000
                                                           : st->vision.error_x1000) % 1000,
                          st->vision.fresh ? "fresh" : "STALE");
}

/* ---- Pairing page (spec 39-41) -----------------------------------------------------------*/

typedef struct {
    lv_obj_t *status;
    lv_obj_t *steps;
    lv_obj_t *btn;
    lv_obj_t *node[3], *node_lbl[3], *wire[2];
    lv_obj_t *big, *key, *token;
} pair_t;

static pair_t s_pair;

static void pair_cb(lv_event_t *e)
{
    scr_link_request_pair();
}

static void pair_back_cb(lv_event_t *e)
{
    (void)e;
    ui_nav_open(UI_PAGE_SETTINGS);
}

/* Pairing = the three-step handshake drawn as a circuit: each node lights up
 * once satisfied (car in range, pairing requested, token stored), so the user
 * sees where the process stands instead of reading a paragraph. */
void ui_pages_create_pair(lv_obj_t *root)
{
    ui_header(root, "PAIRING", pair_back_cb);
    static const char *const sn[3] = { "CAR IN RANGE", "HOLD CAR BUTTON 3S", "TAP PAIR" };
    static const char *const si[3] = { LV_SYMBOL_WIFI, LV_SYMBOL_POWER, LV_SYMBOL_OK };
    for (int i = 0; i < 3; i++) {
        int x = 70 + i * 250;
        lv_obj_t *n = lv_obj_create(root);
        lv_obj_set_size(n, 90, 90);
        lv_obj_set_pos(n, x, 76);
        lv_obj_set_style_radius(n, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(n, lv_color_hex(UI_COL_SURFACE), 0);
        lv_obj_set_style_bg_opa(n, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(n, 2, 0);
        lv_obj_set_style_border_color(n, lv_color_hex(UI_COL_LINE), 0);
        lv_obj_remove_flag(n, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(n, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *ic = lv_label_create(n);
        lv_label_set_text(ic, si[i]);
        lv_obj_set_style_text_font(ic, F_XL, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(UI_COL_DIM), 0);
        lv_obj_center(ic);
        s_pair.node[i] = n;
        lv_obj_t *nl = lv_label_create(root);
        lv_label_set_text_fmt(nl, "%02d  %s", i + 1, sn[i]);
        lv_obj_set_style_text_font(nl, F_SM, 0);
        lv_obj_set_style_text_color(nl, lv_color_hex(UI_COL_DIM), 0);
        lv_obj_set_style_text_letter_space(nl, 1, 0);
        lv_obj_set_width(nl, 220);
        lv_obj_set_style_text_align(nl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(nl, x + 45 - 110, 176);
        s_pair.node_lbl[i] = nl;
        if (i < 2) {
            lv_obj_t *wv = lv_obj_create(root);
            lv_obj_set_size(wv, 160, 3);
            lv_obj_set_pos(wv, x + 90, 120);
            lv_obj_set_style_radius(wv, 0, 0);
            lv_obj_set_style_border_width(wv, 0, 0);
            lv_obj_set_style_bg_color(wv, lv_color_hex(UI_COL_LINE), 0);
            lv_obj_set_style_bg_opa(wv, LV_OPA_COVER, 0);
            lv_obj_remove_flag(wv, LV_OBJ_FLAG_CLICKABLE);
            s_pair.wire[i] = wv;
        }
    }

    lv_obj_t *card = ui_card(root);
    lv_obj_set_size(card, 784, 262);
    lv_obj_set_pos(card, 8, 210);
    lv_obj_set_style_pad_all(card, 16, 0);
    s_pair.key = lv_label_create(card);
    lv_label_set_text(s_pair.key, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_font(s_pair.key, F_XXL, 0);
    lv_obj_set_style_text_color(s_pair.key, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_align(s_pair.key, LV_ALIGN_TOP_LEFT, 0, 4);
    s_pair.big = lv_label_create(card);
    lv_label_set_text(s_pair.big, "NOT PAIRED");
    lv_obj_set_style_text_font(s_pair.big, F_XL, 0);
    lv_obj_set_style_text_letter_space(s_pair.big, 3, 0);
    lv_obj_set_style_text_color(s_pair.big, lv_color_hex(UI_COL_WARN), 0);
    lv_obj_align(s_pair.big, LV_ALIGN_TOP_LEFT, 78, 4);
    s_pair.token = lv_label_create(card);
    lv_label_set_text(s_pair.token, " ");
    lv_obj_set_style_text_font(s_pair.token, F_SM, 0);
    lv_obj_set_style_text_color(s_pair.token, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_align(s_pair.token, LV_ALIGN_TOP_LEFT, 80, 44);
    s_pair.status = lv_label_create(card);
    lv_label_set_text(s_pair.status, " ");
    lv_obj_set_style_text_font(s_pair.status, F_MD, 0);
    lv_obj_set_style_text_color(s_pair.status, lv_color_hex(UI_COL_TXT), 0);
    lv_obj_set_width(s_pair.status, 752);
    lv_label_set_long_mode(s_pair.status, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_pair.status, LV_ALIGN_TOP_LEFT, 0, 86);
    s_pair.steps = s_pair.status;       /* legacy handle */
    s_pair.btn = ui_button(card, LV_SYMBOL_BLUETOOTH "  PAIR WITH VEHICLE",
                           lv_color_hex(0x0B2A3A), pair_cb, NULL);
    lv_obj_set_size(s_pair.btn, LV_PCT(100), 60);
    lv_obj_set_style_border_width(s_pair.btn, 2, 0);
    lv_obj_set_style_border_color(s_pair.btn, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_text_font(lv_obj_get_child(s_pair.btn, 0), F_LG, 0);
    lv_obj_align(s_pair.btn, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void pair_node(int i, bool on)
{
    lv_color_t c = lv_color_hex(on ? UI_COL_ACCENT : UI_COL_LINE);
    if (!lv_color_eq(lv_obj_get_style_border_color(s_pair.node[i], 0), c)) {
        lv_obj_set_style_border_color(s_pair.node[i], c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(s_pair.node[i], 0),
                                    lv_color_hex(on ? UI_COL_ACCENT : UI_COL_DIM), 0);
        lv_obj_set_style_text_color(s_pair.node_lbl[i],
                                    lv_color_hex(on ? UI_COL_TXT : UI_COL_DIM), 0);
        if (i < 2) {
            lv_obj_set_style_bg_color(s_pair.wire[i], c, 0);
        }
    }
}

void ui_pages_pair_refresh(const scr_state_t *st)
{
    scr_settings_t set;
    scr_settings_get(&set);
    bool paired = set.token[0] != '\0';
    bool in_range = st->conn == SCR_CONN_CONNECTED;
    bool asked = st->pair_status[0] != '\0';

    pair_node(0, in_range);
    pair_node(1, in_range && (asked || paired));
    pair_node(2, paired);

    ui_label_set_text(s_pair.big, paired ? "PAIRED" : "NOT PAIRED");
    ui_label_set_color(s_pair.big, lv_color_hex(paired ? UI_COL_OK : UI_COL_WARN));
    ui_label_set_text(s_pair.key, paired ? LV_SYMBOL_OK : LV_SYMBOL_EYE_CLOSE);
    ui_label_set_color(s_pair.key, lv_color_hex(paired ? UI_COL_OK : UI_COL_DIM));
    if (paired) {
        size_t n = strlen(set.token);
        ui_label_set_fmt(s_pair.token, "TOKEN  %.4s ****** %s", set.token,
                         n > 8 ? set.token + n - 4 : "");
    } else {
        ui_label_set_text(s_pair.token, "No session token stored");
    }
    ui_label_set_text(s_pair.status, asked ? st->pair_status :
                      (!in_range ? "Power on the car and come within range of its hotspot." :
                       paired ? "This remote is bound to the car. Pair again only after a reset." :
                       "Hold the pairing button on the car for 3 s, then tap PAIR within the window."));
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
    /* control sub: drive tuning */
    lv_obj_t *mode_btns[3];
    lv_obj_t *mode_bar[3];
    lv_obj_t *dz_lbl;
    lv_obj_t *dz_slider;
    lv_obj_t *dz_ring;          /* dead-zone disc inside the stick outline */
    lv_obj_t *curve;            /* response curve polyline               */
    lv_point_precise_t curve_pts[4];
    lv_obj_t *curve_txt;
    int       ctl_mode_drawn, ctl_dz_drawn;
    /* radio sub: link console */
    lv_obj_t *rd_bar[5];
    lv_obj_t *rd_rssi, *rd_q, *rd_ch, *rd_lat, *rd_loss, *rd_rate, *rd_ssid;
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
    lv_obj_t *tile_sub[SUB_NAV_FDIAG + 1];   /* live subtitle per module */
    /* about sub: system topology */
    lv_obj_t *ab_ver[3], *ab_st[3], *ab_dot[3];
    lv_obj_t *ab_sbl;           /* TC275 card only: second row, SBL version */
    lv_obj_t *ab_wire[2], *ab_wlbl[2], *ab_wst[2];
    lv_obj_t *ab_up, *ab_heap, *ab_psram, *ab_rtt;
    lv_obj_t *fw_val;
    int fw_taps;
    int64_t fw_first_tap;
    /* tap-to-refresh firmware version per topology node (S3 / C6 / TC275) */
    uint8_t  ver_phase[3];      /* VER_IDLE / VER_CHECKING / VER_OK / VER_FAIL */
    uint32_t ver_seq0[3];       /* app_state version seq when the tap landed  */
    int64_t  ver_until[3];      /* CHECKING: timeout; OK/FAIL: banner end (ms) */
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
    if (s == SUB_NAV_FW) {
        ui_nav_open(UI_PAGE_FW);
        return;
    }
    if (s == SUB_NAV_CALIB) {
        ui_nav_open(UI_PAGE_CALIB);
        return;
    }
    if (s == SUB_NAV_FDIAG) {
        ui_nav_open(UI_PAGE_FDIAG);
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
    ui_label_set_fmt(s_set.dz_lbl, "%d%%", v);
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

/* about sub: tap a node card -> re-fetch that firmware's version.
 *   S3    : local esp_app_desc, resolved on the spot
 *   C6    : GET /api/health via the link task, wait for c6_fw_seq to move
 *   TC275 : {"t":"tcver"} -> C6 -> SPI DIAG 0x53/0x24 (IRQ line notifies the
 *           TC275 master) -> EVT 0x24/0x25 -> {"t":"tcver"}; wait for
 *           tc_ver_seq to move.  The timeout still covers one 5 s periodic
 *           beacon, so a TC275 without the request handler also resolves */
enum { VER_IDLE = 0, VER_CHECKING, VER_OK, VER_FAIL };
#define VER_C6_TIMEOUT_MS   3000
#define VER_TC_TIMEOUT_MS   6500    /* > one 5 s beacon period */
#define VER_BANNER_MS       1500

static void node_tap_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    int64_t now = esp_timer_get_time() / 1000;
    if (i == 0) {
        fw_tap_cb(e);               /* keep the 7-tap engineer unlock on S3 */
    }
    if (s_set.ver_phase[i] == VER_CHECKING) {
        return;                     /* one query in flight per node */
    }
    scr_state_t st;
    app_state_snapshot(&st);
    bool ws = st.conn == SCR_CONN_CONNECTED;
    if (i == 0) {
        s_set.ver_phase[0] = VER_OK;            /* local: always immediate */
        s_set.ver_until[0] = now + VER_BANNER_MS;
    } else if (!ws || (i == 2 && !st.tc_on)) {
        s_set.ver_phase[i] = VER_FAIL;          /* nothing to ask */
        s_set.ver_until[i] = now + VER_BANNER_MS;
    } else {
        s_set.ver_phase[i] = VER_CHECKING;
        s_set.ver_seq0[i] = i == 1 ? st.c6_fw_seq : st.tc_ver_seq;
        s_set.ver_until[i] = now + (i == 1 ? VER_C6_TIMEOUT_MS : VER_TC_TIMEOUT_MS);
        if (i == 1) {
            scr_link_request_c6_ver();
        } else {
            scr_link_request_tc_ver();
        }
    }
    app_state_log(SCR_LOG_INFO, i == 0 ? "Version refresh: S3" :
                  i == 1 ? "Version refresh: C6" : "Version refresh: TC275");
}

/* advance a node's refresh state; returns the status override or NULL */
static const char *node_ver_status(int i, const scr_state_t *st, uint32_t *col)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (s_set.ver_phase[i] == VER_CHECKING) {
        uint32_t seq = i == 1 ? st->c6_fw_seq : st->tc_ver_seq;
        if (seq != s_set.ver_seq0[i]) {
            s_set.ver_phase[i] = VER_OK;
            s_set.ver_until[i] = now + VER_BANNER_MS;
        } else if (now >= s_set.ver_until[i]) {
            s_set.ver_phase[i] = VER_FAIL;
            s_set.ver_until[i] = now + VER_BANNER_MS;
        } else {
            *col = UI_COL_WARN;
            return "CHECKING...";
        }
    }
    if (s_set.ver_phase[i] == VER_OK || s_set.ver_phase[i] == VER_FAIL) {
        if (now >= s_set.ver_until[i]) {
            s_set.ver_phase[i] = VER_IDLE;
            return NULL;
        }
        bool ok = s_set.ver_phase[i] == VER_OK;
        *col = ok ? UI_COL_ACCENT : UI_COL_CRIT;
        return ok ? "UPDATED" : (i == 1 ? "NO REPLY" : "NO BEACON");
    }
    return NULL;
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
    /* fixed third of the row: free-width chips with padding + letter spacing
     * summed past the 246 px row and the outer two were cut off */
    lv_obj_set_width(c, LV_PCT(32));
    lv_label_set_long_mode(c, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_hor(c, 0, 0);
    lv_obj_set_style_pad_ver(c, 4, 0);
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
    /* DOTS only truncates with a FIXED height: with auto height a long value
     * ("NORMAL - dead zone 10%") wraps to 2 lines and, bottom-aligned, grows
     * up into the title (tiles 01/04 overlap). One line, dots at the end. */
    lv_obj_set_size(sub, LV_PCT(66), lv_font_get_line_height(F_SM));
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
    lv_obj_set_style_pad_column(chips, 0, 0);
    lv_obj_remove_flag(chips, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chips, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
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
    module_tile(items, idx++, LV_SYMBOL_GPS,     "CONTROL", SUB_CONTROL);
    module_tile(items, idx++, LV_SYMBOL_WIFI,    "RADIO",   SUB_RADIO);
    module_tile(items, idx++, LV_SYMBOL_BLUETOOTH, "PAIRING", SUB_NAV_PAIR);
    module_tile(items, idx++, LV_SYMBOL_LIST,    "SYSTEM",  SUB_ABOUT);
    module_tile(items, idx++, LV_SYMBOL_UPLOAD,  "FIRMWARE", SUB_NAV_FW);
    module_tile(items, idx++, LV_SYMBOL_SHUFFLE, "CALIBRATE", SUB_NAV_CALIB);
    module_tile(items, idx++, LV_SYMBOL_WARNING, "DIAGNOSE", SUB_NAV_FDIAG);
    if (ui_engineer_mode()) {
        module_tile(items, idx++, LV_SYMBOL_SETTINGS, "DIAG",   SUB_NAV_DIAG);
        module_tile(items, idx++, LV_SYMBOL_FILE,     "EVENTS", SUB_NAV_EVENTS);
    }
    /* static tile subtitles; live ones are written by the refresh */
    ui_label_set_text(s_set.tile_sub[SUB_NAV_PAIR], "Bind to a car");
    ui_label_set_text(s_set.tile_sub[SUB_NAV_CALIB], "TC275 encoders / jog");
    ui_label_set_text(s_set.tile_sub[SUB_NAV_FDIAG], "C6 + TC275 health");
    if (s_set.tile_sub[SUB_NAV_DIAG]) {
        ui_label_set_text(s_set.tile_sub[SUB_NAV_DIAG], "Link + vehicle internals");
        ui_label_set_text(s_set.tile_sub[SUB_NAV_EVENTS], "Event log");
    }

    /* ---- Control sub = DRIVE TUNING (spec 32/33) ----
     * Three mode cards showing what each mode does (output cap as a bar), a
     * to-scale dead-zone disc inside the stick outline, and the resulting
     * response curve: the user sees the effect of a setting, not a number. */
    s_set.subs[SUB_CONTROL] = make_sub(root);
    lv_obj_t *cs = s_set.subs[SUB_CONTROL];
    ui_header(cs, "DRIVE TUNING", sub_back_cb);
    static const char *const mname[3] = { "ECO", "NORMAL", "SPORT" };
    static const char *const mdesc[3] = { "Gentle, learning", "Balanced daily", "Full power" };
    const int mpct[3] = { CONFIG_SCR_MODE_ECO_PCT, CONFIG_SCR_MODE_NORMAL_PCT,
                          CONFIG_SCR_MODE_SPORT_PCT };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *mb = lv_button_create(cs);
        lv_obj_set_size(mb, 168, 214);
        lv_obj_set_pos(mb, 8 + i * 176, 60);
        lv_obj_set_style_radius(mb, 8, 0);
        lv_obj_set_style_shadow_width(mb, 0, 0);
        lv_obj_set_style_pad_all(mb, 12, 0);
        lv_obj_set_style_bg_color(mb, lv_color_hex(UI_COL_SURFACE), 0);
        lv_obj_set_style_border_width(mb, 1, 0);
        lv_obj_set_style_border_color(mb, lv_color_hex(UI_COL_LINE), 0);
        lv_obj_set_style_bg_color(mb, lv_color_hex(0x0B2A3A), LV_STATE_CHECKED);
        lv_obj_set_style_border_color(mb, lv_color_hex(UI_COL_ACCENT), LV_STATE_CHECKED);
        lv_obj_set_style_border_width(mb, 2, LV_STATE_CHECKED);
        lv_obj_add_event_cb(mb, mode_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *ix = lv_label_create(mb);
        lv_label_set_text_fmt(ix, "MODE %02d", i + 1);
        lv_obj_set_style_text_font(ix, F_SM, 0);
        lv_obj_set_style_text_color(ix, lv_color_hex(UI_COL_ACCENT), 0);
        lv_obj_set_style_text_letter_space(ix, 2, 0);
        lv_obj_align(ix, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *nm = lv_label_create(mb);
        lv_label_set_text(nm, mname[i]);
        lv_obj_set_style_text_font(nm, F_XL, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(UI_COL_TXT), 0);
        lv_obj_set_style_text_letter_space(nm, 2, 0);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 0, 26);
        lv_obj_t *ds = lv_label_create(mb);
        lv_label_set_text(ds, mdesc[i]);
        lv_obj_set_style_text_font(ds, F_SM, 0);
        lv_obj_set_style_text_color(ds, lv_color_hex(UI_COL_DIM), 0);
        lv_obj_align(ds, LV_ALIGN_TOP_LEFT, 0, 64);
        lv_obj_t *pc = lv_label_create(mb);
        lv_label_set_text_fmt(pc, "%d%%", mpct[i]);
        lv_obj_set_style_text_font(pc, F_XXL, 0);
        lv_obj_set_style_text_color(pc, lv_color_hex(UI_COL_TXT), 0);
        lv_obj_align(pc, LV_ALIGN_BOTTOM_LEFT, 0, -22);
        lv_obj_t *cap = lv_label_create(mb);
        lv_label_set_text(cap, "OUTPUT CAP");
        lv_obj_set_style_text_font(cap, F_SM, 0);
        lv_obj_set_style_text_color(cap, lv_color_hex(UI_COL_DIM), 0);
        lv_obj_align(cap, LV_ALIGN_BOTTOM_RIGHT, 0, -30);
        lv_obj_t *br = lv_bar_create(mb);
        lv_obj_set_size(br, LV_PCT(100), 8);
        lv_bar_set_range(br, 0, 100);
        lv_bar_set_value(br, mpct[i], LV_ANIM_OFF);
        lv_obj_set_style_radius(br, 2, 0);
        lv_obj_set_style_radius(br, 2, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(br, lv_color_hex(UI_COL_SURFACE2), 0);
        lv_obj_set_style_bg_color(br, lv_color_hex(UI_COL_DIM), LV_PART_INDICATOR);
        lv_obj_remove_flag(br, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(br, LV_ALIGN_BOTTOM_MID, 0, 0);
        s_set.mode_btns[i] = mb;
        s_set.mode_bar[i] = br;
    }

    /* dead zone: to-scale disc inside the stick outline + slider */
    lv_obj_t *dzc = ui_card(cs);
    lv_obj_set_size(dzc, 256, 214);
    lv_obj_set_pos(dzc, 536, 60);
    lv_obj_set_style_pad_all(dzc, 12, 0);
    lv_obj_t *dzt = lv_label_create(dzc);
    lv_label_set_text(dzt, "DEAD ZONE");
    lv_obj_set_style_text_font(dzt, F_SM, 0);
    lv_obj_set_style_text_color(dzt, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_text_letter_space(dzt, 3, 0);
    lv_obj_align(dzt, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *outline = lv_obj_create(dzc);
    lv_obj_set_size(outline, 120, 120);
    lv_obj_set_style_radius(outline, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(outline, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(outline, 2, 0);
    lv_obj_set_style_border_color(outline, lv_color_hex(UI_COL_LINE), 0);
    lv_obj_set_style_pad_all(outline, 0, 0);
    lv_obj_remove_flag(outline, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(outline, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(outline, LV_ALIGN_TOP_LEFT, 0, 26);
    s_set.dz_ring = lv_obj_create(outline);
    lv_obj_set_size(s_set.dz_ring, 12, 12);
    lv_obj_set_style_radius(s_set.dz_ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_set.dz_ring, 1, 0);
    lv_obj_set_style_border_color(s_set.dz_ring, lv_color_hex(UI_COL_WARN), 0);
    lv_obj_set_style_bg_color(s_set.dz_ring, lv_color_hex(UI_COL_WARN), 0);
    lv_obj_set_style_bg_opa(s_set.dz_ring, LV_OPA_30, 0);
    lv_obj_remove_flag(s_set.dz_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(s_set.dz_ring);
    s_set.dz_lbl = lv_label_create(dzc);
    lv_label_set_text(s_set.dz_lbl, "--");
    lv_obj_set_style_text_font(s_set.dz_lbl, F_XXL, 0);
    lv_obj_set_style_text_color(s_set.dz_lbl, lv_color_hex(UI_COL_TXT), 0);
    lv_obj_align(s_set.dz_lbl, LV_ALIGN_TOP_RIGHT, 0, 40);
    lv_obj_t *dzn = lv_label_create(dzc);
    lv_label_set_text(dzn, "of stick\nradius");
    lv_obj_set_style_text_font(dzn, F_SM, 0);
    lv_obj_set_style_text_color(dzn, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_set_style_text_align(dzn, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(dzn, LV_ALIGN_TOP_RIGHT, 0, 100);
    lv_obj_t *dz_sl = lv_slider_create(dzc);
    s_set.dz_slider = dz_sl;
    lv_obj_set_size(dz_sl, LV_PCT(94), 10);
    lv_obj_align(dz_sl, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_slider_set_range(dz_sl, 0, 30);
    lv_obj_set_style_bg_color(dz_sl, lv_color_hex(UI_COL_SURFACE2), 0);
    lv_obj_set_style_bg_color(dz_sl, lv_color_hex(UI_COL_WARN), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(dz_sl, lv_color_hex(UI_COL_TXT), LV_PART_KNOB);
    lv_obj_add_event_cb(dz_sl, dz_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(dz_sl, dz_released_cb, LV_EVENT_RELEASED, NULL);

    /* response curve: stick deflection -> output of the selected mode */
    lv_obj_t *rc = ui_card(cs);
    lv_obj_set_size(rc, 784, 190);
    lv_obj_set_pos(rc, 8, 282);
    lv_obj_set_style_pad_all(rc, 12, 0);
    lv_obj_t *rct = lv_label_create(rc);
    lv_label_set_text(rct, "RESPONSE CURVE");
    lv_obj_set_style_text_font(rct, F_SM, 0);
    lv_obj_set_style_text_color(rct, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_text_letter_space(rct, 3, 0);
    lv_obj_align(rct, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *plot = lv_obj_create(rc);
    lv_obj_set_size(plot, 520, 130);
    lv_obj_align(plot, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(plot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(plot, 1, 0);
    lv_obj_set_style_border_side(plot, LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(plot, lv_color_hex(UI_COL_LINE), 0);
    lv_obj_set_style_pad_all(plot, 0, 0);
    lv_obj_remove_flag(plot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(plot, LV_OBJ_FLAG_CLICKABLE);
    for (int g = 1; g < 4; g++) {       /* faint 25/50/75 % grid */
        lv_obj_t *gl = lv_obj_create(plot);
        lv_obj_set_size(gl, 520, 1);
        lv_obj_set_style_border_width(gl, 0, 0);
        lv_obj_set_style_bg_color(gl, lv_color_hex(UI_COL_SURFACE2), 0);
        lv_obj_set_style_bg_opa(gl, LV_OPA_COVER, 0);
        lv_obj_remove_flag(gl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_pos(gl, 0, 130 - g * 130 / 4);
    }
    s_set.curve = lv_line_create(plot);
    lv_obj_set_style_line_width(s_set.curve, 3, 0);
    lv_obj_set_style_line_color(s_set.curve, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_line_rounded(s_set.curve, true, 0);
    lv_obj_set_pos(s_set.curve, 0, 0);
    lv_obj_t *xl = lv_label_create(rc);
    lv_label_set_text(xl, "STICK " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(xl, F_SM, 0);
    lv_obj_set_style_text_color(xl, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_align(xl, LV_ALIGN_TOP_LEFT, 440, 0);
    s_set.curve_txt = lv_label_create(rc);
    lv_label_set_text(s_set.curve_txt, " ");
    lv_obj_set_width(s_set.curve_txt, 220);
    lv_label_set_long_mode(s_set.curve_txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_set.curve_txt, F_MD, 0);
    lv_obj_set_style_text_color(s_set.curve_txt, lv_color_hex(UI_COL_TXT), 0);
    lv_obj_set_style_text_line_space(s_set.curve_txt, 6, 0);
    lv_obj_align(s_set.curve_txt, LV_ALIGN_TOP_RIGHT, 0, 24);
    s_set.ctl_mode_drawn = s_set.ctl_dz_drawn = -1;

    /* ---- Radio sub = RADIO LINK console (spec 35) ----
     * Left: the live link (signal bars, dBm, channel, RTT, loss, rates), so
     * the effect of a credential change is visible on the same screen.
     * Right: hotspot credentials + pairing.  Keyboard slides up as before. */
    s_set.subs[SUB_RADIO] = make_sub(root);
    lv_obj_t *rs = s_set.subs[SUB_RADIO];
    ui_header(rs, "RADIO LINK", sub_back_cb);
    lv_obj_t *lc = ui_card(rs);
    lv_obj_set_size(lc, 300, 412);
    lv_obj_set_pos(lc, 8, 60);
    lv_obj_set_style_pad_all(lc, 12, 0);
    lv_obj_t *lt = lv_label_create(lc);
    lv_label_set_text(lt, "LIVE SIGNAL");
    lv_obj_set_style_text_font(lt, F_SM, 0);
    lv_obj_set_style_text_color(lt, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_text_letter_space(lt, 3, 0);
    lv_obj_align(lt, LV_ALIGN_TOP_LEFT, 0, 0);
    for (int i = 0; i < 5; i++) {
        s_set.rd_bar[i] = lv_obj_create(lc);
        int bh = 20 + i * 18;
        lv_obj_set_size(s_set.rd_bar[i], 22, bh);
        lv_obj_set_pos(s_set.rd_bar[i], i * 30, 132 - bh);
        lv_obj_set_style_radius(s_set.rd_bar[i], 3, 0);
        lv_obj_set_style_border_width(s_set.rd_bar[i], 0, 0);
        lv_obj_set_style_bg_color(s_set.rd_bar[i], lv_color_hex(UI_COL_SURFACE2), 0);
        lv_obj_set_style_bg_opa(s_set.rd_bar[i], LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_set.rd_bar[i], LV_OBJ_FLAG_CLICKABLE);
    }
    s_set.rd_rssi = lv_label_create(lc);
    lv_label_set_text(s_set.rd_rssi, "--");
    lv_obj_set_style_text_font(s_set.rd_rssi, F_XXL, 0);
    lv_obj_set_style_text_color(s_set.rd_rssi, lv_color_hex(UI_COL_TXT), 0);
    lv_obj_align(s_set.rd_rssi, LV_ALIGN_TOP_RIGHT, 0, 28);
    lv_obj_t *du = lv_label_create(lc);
    lv_label_set_text(du, "dBm");
    lv_obj_set_style_text_font(du, F_SM, 0);
    lv_obj_set_style_text_color(du, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_align(du, LV_ALIGN_TOP_RIGHT, 0, 88);
    s_set.rd_q = lv_label_create(lc);
    lv_label_set_text(s_set.rd_q, "NO SIGNAL");
    lv_obj_set_style_text_font(s_set.rd_q, F_LG, 0);
    lv_obj_set_style_text_letter_space(s_set.rd_q, 3, 0);
    lv_obj_set_style_text_color(s_set.rd_q, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_align(s_set.rd_q, LV_ALIGN_TOP_LEFT, 0, 146);
    static const char *const rk[5] = { "NETWORK", "CHANNEL", "ROUND TRIP", "LOSS", "TX / RX" };
    lv_obj_t **rv[5] = { &s_set.rd_ssid, &s_set.rd_ch, &s_set.rd_lat, &s_set.rd_loss, &s_set.rd_rate };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *k = lv_label_create(lc);
        lv_label_set_text(k, rk[i]);
        lv_obj_set_style_text_font(k, F_SM, 0);
        lv_obj_set_style_text_color(k, lv_color_hex(UI_COL_DIM), 0);
        lv_obj_align(k, LV_ALIGN_TOP_LEFT, 0, 196 + i * 40);
        *rv[i] = lv_label_create(lc);
        lv_label_set_text(*rv[i], "--");
        lv_obj_set_style_text_font(*rv[i], F_LG, 0);
        lv_obj_set_style_text_color(*rv[i], lv_color_hex(UI_COL_TXT), 0);
        lv_obj_set_size(*rv[i], 150, lv_font_get_line_height(F_LG));
        lv_label_set_long_mode(*rv[i], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(*rv[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(*rv[i], LV_ALIGN_TOP_RIGHT, 0, 192 + i * 40);
    }

    lv_obj_t *cc = ui_card(rs);
    lv_obj_set_size(cc, 476, 412);
    lv_obj_set_pos(cc, 316, 60);
    lv_obj_set_style_pad_all(cc, 14, 0);
    lv_obj_t *hs_t = lv_label_create(cc);
    lv_label_set_text(hs_t, "VEHICLE HOTSPOT");
    lv_obj_set_style_text_font(hs_t, F_SM, 0);
    lv_obj_set_style_text_color(hs_t, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_text_letter_space(hs_t, 3, 0);
    lv_obj_align(hs_t, LV_ALIGN_TOP_LEFT, 0, 0);
    static const char *const fk[2] = { LV_SYMBOL_WIFI "  SSID", LV_SYMBOL_EYE_CLOSE "  PASSWORD" };
    lv_obj_t **ft[2] = { &s_set.ssid_ta, &s_set.pass_ta };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *k = lv_label_create(cc);
        lv_label_set_text(k, fk[i]);
        lv_obj_set_style_text_font(k, F_SM, 0);
        lv_obj_set_style_text_color(k, lv_color_hex(UI_COL_DIM), 0);
        lv_obj_align(k, LV_ALIGN_TOP_LEFT, 0, 30 + i * 80);
        lv_obj_t *ta = lv_textarea_create(cc);
        lv_obj_set_size(ta, LV_PCT(100), 46);
        lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 0, 50 + i * 80);
        lv_textarea_set_one_line(ta, true);
        lv_obj_set_style_bg_color(ta, lv_color_hex(UI_COL_SURFACE2), 0);
        lv_obj_set_style_text_color(ta, lv_color_hex(UI_COL_TXT), 0);
        lv_obj_set_style_text_font(ta, F_MD, 0);
        lv_obj_set_style_border_width(ta, 1, 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(UI_COL_LINE), 0);
        lv_obj_set_style_border_color(ta, lv_color_hex(UI_COL_ACCENT), LV_STATE_FOCUSED);
        lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
        lv_obj_add_event_cb(ta, ta_defocus_cb, LV_EVENT_DEFOCUSED, NULL);
        *ft[i] = ta;
    }
    lv_textarea_set_password_mode(s_set.pass_ta, true);

    lv_obj_t *apply = ui_button(cc, LV_SYMBOL_REFRESH "  APPLY + RECONNECT",
                                lv_color_hex(0x0B2A3A), wifi_apply_cb, NULL);
    lv_obj_set_size(apply, LV_PCT(100), 50);
    lv_obj_set_style_border_color(apply, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_align(apply, LV_ALIGN_TOP_LEFT, 0, 200);
    lv_obj_t *pairb = ui_button(cc, LV_SYMBOL_BLUETOOTH "  PAIR WITH VEHICLE",
                                lv_color_hex(UI_COL_SURFACE2), pair_btn_cb, NULL);
    lv_obj_set_size(pairb, LV_PCT(100), 50);
    lv_obj_align(pairb, LV_ALIGN_TOP_LEFT, 0, 260);
    s_set.pair_status_lbl = lv_label_create(cc);
    lv_label_set_text(s_set.pair_status_lbl, " ");
    lv_obj_set_width(s_set.pair_status_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_set.pair_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_set.pair_status_lbl, F_SM, 0);
    lv_obj_set_style_text_color(s_set.pair_status_lbl, lv_color_hex(UI_COL_INFO), 0);
    lv_obj_align(s_set.pair_status_lbl, LV_ALIGN_TOP_LEFT, 0, 324);

    s_set.kb = lv_keyboard_create(rs);
    lv_obj_set_size(s_set.kb, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_align(s_set.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(s_set.kb, LV_OBJ_FLAG_HIDDEN);


    /* ---- About = SYSTEM topology (spec 45 + engineer trigger) ----
     * Not a spec sheet: the page draws the real chain S3 -> C6 -> TC275 as
     * three nodes joined by the two live links, each node with its running
     * firmware and state, each link coloured by its health.  Every label sets
     * its colour explicitly - the old page inherited the default (dark) text
     * colour and was unreadable on the dark HUD. */
    s_set.subs[SUB_ABOUT] = make_sub(root);
    lv_obj_t *ab = s_set.subs[SUB_ABOUT];
    ui_header(ab, "SYSTEM", sub_back_cb);

    lv_obj_t *brand = lv_label_create(ab);
    lv_label_set_text(brand, "SMART CAR");
    lv_obj_set_style_text_font(brand, F_XXL, 0);
    lv_obj_set_style_text_color(brand, lv_color_hex(UI_COL_TXT), 0);
    lv_obj_set_style_text_letter_space(brand, 10, 0);
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 66);
    lv_obj_t *tag = lv_label_create(ab);
    lv_label_set_text(tag, "REMOTE  //  C6 GATEWAY  //  TC275 VEHICLE");
    lv_obj_set_style_text_font(tag, F_SM, 0);
    lv_obj_set_style_text_color(tag, lv_color_hex(UI_COL_ACCENT), 0);
    lv_obj_set_style_text_letter_space(tag, 3, 0);
    lv_obj_align(tag, LV_ALIGN_TOP_MID, 0, 128);

    static const char *const nname[3] = { "ESP32-S3", "ESP32-C6", "TC275" };
    static const char *const nrole[3] = { "REMOTE / HMI", "GATEWAY / AP", "VEHICLE MCU" };
    static const char *const nicon[3] = { LV_SYMBOL_IMAGE, LV_SYMBOL_WIFI, LV_SYMBOL_DRIVE };
    const int NW = 190, NH = 150, NY = 164, GAP = (800 - 3 * NW) / 4;
    for (int i = 0; i < 3; i++) {
        int x = GAP + i * (NW + GAP);
        lv_obj_t *n = ui_card(ab);
        lv_obj_set_size(n, NW, NH);
        lv_obj_set_pos(n, x, NY);
        lv_obj_set_style_pad_all(n, 10, 0);
        lv_obj_t *ic = lv_label_create(n);
        lv_label_set_text(ic, nicon[i]);
        lv_obj_set_style_text_font(ic, F_XL, 0);
        lv_obj_set_style_text_color(ic, lv_color_hex(UI_COL_ACCENT), 0);
        lv_obj_align(ic, LV_ALIGN_TOP_LEFT, 0, 0);
        s_set.ab_dot[i] = lv_obj_create(n);
        lv_obj_set_size(s_set.ab_dot[i], 12, 12);
        lv_obj_set_style_radius(s_set.ab_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_set.ab_dot[i], 0, 0);
        lv_obj_set_style_bg_color(s_set.ab_dot[i], lv_color_hex(UI_COL_DIM), 0);
        lv_obj_set_style_bg_opa(s_set.ab_dot[i], LV_OPA_COVER, 0);
        lv_obj_remove_flag(s_set.ab_dot[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(s_set.ab_dot[i], LV_ALIGN_TOP_RIGHT, 0, 6);
        lv_obj_t *nm = lv_label_create(n);
        lv_label_set_text(nm, nname[i]);
        lv_obj_set_style_text_font(nm, F_LG, 0);
        lv_obj_set_style_text_color(nm, lv_color_hex(UI_COL_TXT), 0);
        lv_obj_set_style_text_letter_space(nm, 2, 0);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 0, 32);
        lv_obj_t *rl = lv_label_create(n);
        lv_label_set_text(rl, nrole[i]);
        lv_obj_set_style_text_font(rl, F_SM, 0);
        lv_obj_set_style_text_color(rl, lv_color_hex(UI_COL_DIM), 0);
        lv_obj_align(rl, LV_ALIGN_TOP_LEFT, 0, 56);
        s_set.ab_ver[i] = lv_label_create(n);
        lv_label_set_text(s_set.ab_ver[i], "--");
        lv_obj_set_style_text_font(s_set.ab_ver[i], F_MD, 0);
        lv_obj_set_style_text_color(s_set.ab_ver[i], lv_color_hex(UI_COL_TXT), 0);
        lv_obj_set_size(s_set.ab_ver[i], NW - 20, lv_font_get_line_height(F_MD));
        lv_label_set_long_mode(s_set.ab_ver[i], LV_LABEL_LONG_MODE_DOTS);
        /* bottom stack (128 px interior): status F_SM 16 | version rows F_MD 18.
         * TC275 carries two rows - APP above, SBL below - the others one. */
        const int lh_md = lv_font_get_line_height(F_MD);
        const int lh_sm = lv_font_get_line_height(F_SM);
        lv_obj_align(s_set.ab_ver[i], LV_ALIGN_BOTTOM_LEFT, 0,
                     i == 2 ? -(lh_sm + 2 + lh_md) : -(lh_sm + 2));
        if (i == 2) {
            s_set.ab_sbl = lv_label_create(n);
            lv_label_set_text(s_set.ab_sbl, "SBL  --");
            lv_obj_set_style_text_font(s_set.ab_sbl, F_MD, 0);
            lv_obj_set_style_text_color(s_set.ab_sbl, lv_color_hex(UI_COL_TXT), 0);
            lv_obj_set_size(s_set.ab_sbl, NW - 20, lh_md);
            lv_label_set_long_mode(s_set.ab_sbl, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_align(s_set.ab_sbl, LV_ALIGN_BOTTOM_LEFT, 0, -(lh_sm + 2));
        }
        s_set.ab_st[i] = lv_label_create(n);
        lv_label_set_text(s_set.ab_st[i], "--");
        lv_obj_set_style_text_font(s_set.ab_st[i], F_SM, 0);
        lv_obj_set_style_text_color(s_set.ab_st[i], lv_color_hex(UI_COL_DIM), 0);
        lv_obj_set_style_text_letter_space(s_set.ab_st[i], 2, 0);
        lv_obj_align(s_set.ab_st[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
        /* every node card is a tap-to-refresh-version button; the S3 card
         * also still counts the 7-tap engineer unlock (inside node_tap_cb) */
        if (i == 0) {
            s_set.fw_val = s_set.ab_ver[0];
        }
        lv_obj_add_flag(n, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_color(n, lv_color_hex(UI_COL_ACCENT), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(n, lv_color_hex(UI_COL_SURFACE2), LV_STATE_PRESSED);
        lv_obj_add_event_cb(n, node_tap_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        if (i < 2) {
            /* live link between node i and i+1 */
            s_set.ab_wire[i] = lv_obj_create(ab);
            lv_obj_set_size(s_set.ab_wire[i], GAP, 3);
            lv_obj_set_pos(s_set.ab_wire[i], x + NW, NY + NH / 2 - 1);
            lv_obj_set_style_radius(s_set.ab_wire[i], 0, 0);
            lv_obj_set_style_border_width(s_set.ab_wire[i], 0, 0);
            lv_obj_set_style_bg_color(s_set.ab_wire[i], lv_color_hex(UI_COL_LINE), 0);
            lv_obj_set_style_bg_opa(s_set.ab_wire[i], LV_OPA_COVER, 0);
            lv_obj_remove_flag(s_set.ab_wire[i], LV_OBJ_FLAG_CLICKABLE);
            s_set.ab_wlbl[i] = lv_label_create(ab);
            lv_label_set_text(s_set.ab_wlbl[i], i == 0 ? "WS" : "SPI");
            lv_obj_set_style_text_font(s_set.ab_wlbl[i], F_SM, 0);
            lv_obj_set_style_text_color(s_set.ab_wlbl[i], lv_color_hex(UI_COL_DIM), 0);
            /* the gap between nodes is only ~57 px, too narrow for
             * "SPI  DOWN" on one line: link name sits above the wire and
             * its state below, each a fixed one-line box clear of the wire */
            const int lh = lv_font_get_line_height(F_SM);
            const int wy = NY + NH / 2 - 1;          /* wire top */
            lv_obj_set_size(s_set.ab_wlbl[i], GAP, lh);
            lv_label_set_long_mode(s_set.ab_wlbl[i], LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_style_text_align(s_set.ab_wlbl[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(s_set.ab_wlbl[i], x + NW, wy - lh - 6);
            s_set.ab_wst[i] = lv_label_create(ab);
            lv_label_set_text(s_set.ab_wst[i], "--");
            lv_obj_set_style_text_font(s_set.ab_wst[i], F_SM, 0);
            lv_obj_set_style_text_color(s_set.ab_wst[i], lv_color_hex(UI_COL_DIM), 0);
            lv_obj_set_size(s_set.ab_wst[i], GAP, lh);
            lv_label_set_long_mode(s_set.ab_wst[i], LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_style_text_align(s_set.ab_wst[i], LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(s_set.ab_wst[i], x + NW, wy + 3 + 6);
        }
    }

    /* live telemetry strip */
    static const char *const tk[4] = { "UPTIME", "INTERNAL RAM", "PSRAM", "LINK RTT" };
    lv_obj_t **tvals[4] = { &s_set.ab_up, &s_set.ab_heap, &s_set.ab_psram, &s_set.ab_rtt };
    lv_obj_t *strip = ui_card(ab);
    lv_obj_set_size(strip, 800 - 2 * GAP, 104);
    lv_obj_set_pos(strip, GAP, 330);
    lv_obj_set_style_pad_all(strip, 12, 0);
    for (int i = 0; i < 4; i++) {
        int cw = (800 - 2 * GAP - 24) / 4;
        lv_obj_t *k = lv_label_create(strip);
        lv_label_set_text(k, tk[i]);
        lv_obj_set_style_text_font(k, F_SM, 0);
        lv_obj_set_style_text_color(k, lv_color_hex(UI_COL_ACCENT), 0);
        lv_obj_set_style_text_letter_space(k, 2, 0);
        lv_obj_set_pos(k, i * cw, 4);
        *tvals[i] = lv_label_create(strip);
        lv_label_set_text(*tvals[i], "--");
        lv_obj_set_style_text_font(*tvals[i], F_XL, 0);
        lv_obj_set_style_text_color(*tvals[i], lv_color_hex(UI_COL_TXT), 0);
        lv_obj_set_pos(*tvals[i], i * cw, 34);
    }
    const esp_app_desc_t *app = esp_app_get_description();
    lv_obj_t *build = lv_label_create(ab);
    lv_label_set_text_fmt(build, "BUILD %s %s  /  IDF %s  /  TAP S3 x7 FOR ENGINEER MODE",
                          app->date, app->time, app->idf_ver);
    lv_obj_set_style_text_font(build, F_SM, 0);
    lv_obj_set_style_text_color(build, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_align(build, LV_ALIGN_BOTTOM_MID, 0, -12);
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
        ui_label_set_fmt(s_set.dz_lbl, "%d%%", set.deadzone_pct);
        s_set.ctl_mode_drawn = s_set.ctl_dz_drawn = -1;
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
    ui_label_set_fmt(s_set.tile_sub[SUB_CONTROL], "%s  /  DZ %u%%",
                     mtxt[set.mode <= 2 ? set.mode : 1], set.deadzone_pct);
    ui_label_set_fmt(s_set.tile_sub[SUB_RADIO], "%s", set.ssid[0] ? set.ssid : "not set");
    ui_label_set_fmt(s_set.tile_sub[SUB_ABOUT], "Topology  /  v%s", app->version);
    static const char *const sst[3] = { "empty", "invalid", "ready" };
    svc_stage_t c6s, tcs;
    scr_svc_get_stage(SVC_FW_C6, &c6s);
    scr_svc_get_stage(SVC_FW_TC, &tcs);
    ui_label_set_fmt(s_set.tile_sub[SUB_NAV_FW], "C6 %s / TC %s", sst[c6s.state], sst[tcs.state]);
}

static void node_set(int i, const char *ver, const char *state, uint32_t col)
{
    ui_label_set_text(s_set.ab_ver[i], ver);
    ui_label_set_text(s_set.ab_st[i], state);
    ui_label_set_color(s_set.ab_st[i], lv_color_hex(col));
    if (!lv_color_eq(lv_obj_get_style_bg_color(s_set.ab_dot[i], 0), lv_color_hex(col))) {
        lv_obj_set_style_bg_color(s_set.ab_dot[i], lv_color_hex(col), 0);
    }
}

static void wire_set(int i, bool up, const char *txt)
{
    uint32_t c = up ? UI_COL_ACCENT : UI_COL_LINE;
    if (!lv_color_eq(lv_obj_get_style_bg_color(s_set.ab_wire[i], 0), lv_color_hex(c))) {
        lv_obj_set_style_bg_color(s_set.ab_wire[i], lv_color_hex(c), 0);
    }
    lv_color_t tc = lv_color_hex(up ? UI_COL_ACCENT : UI_COL_DIM);
    ui_label_set_color(s_set.ab_wlbl[i], tc);
    ui_label_set_text(s_set.ab_wst[i], txt);
    ui_label_set_color(s_set.ab_wst[i], tc);
}

static void control_refresh(void)
{
    scr_settings_t set;
    scr_settings_get(&set);
    int m = set.mode <= 2 ? set.mode : 1;
    int dz = s_set.dz_slider ? lv_slider_get_value(s_set.dz_slider) : set.deadzone_pct;
    if (m == s_set.ctl_mode_drawn && dz == s_set.ctl_dz_drawn) {
        return;                         /* nothing changed: no repaint */
    }
    const int mpct[3] = { CONFIG_SCR_MODE_ECO_PCT, CONFIG_SCR_MODE_NORMAL_PCT,
                          CONFIG_SCR_MODE_SPORT_PCT };
    for (int i = 0; i < 3; i++) {
        lv_obj_set_style_bg_color(s_set.mode_bar[i],
                                  lv_color_hex(i == m ? UI_COL_ACCENT : UI_COL_DIM),
                                  LV_PART_INDICATOR);
    }
    /* disc diameter = dz % of the 116 px stick interior (to scale) */
    int d = 116 * dz / 100;
    d = d < 6 ? 6 : d;
    lv_obj_set_size(s_set.dz_ring, d, d);
    lv_obj_center(s_set.dz_ring);
    /* curve: flat inside the dead zone, then linear up to the mode cap */
    const int W = 520, H = 130;
    s_set.curve_pts[0] = (lv_point_precise_t){ 0, H };
    s_set.curve_pts[1] = (lv_point_precise_t){ W * dz / 100, H };
    s_set.curve_pts[2] = (lv_point_precise_t){ W, H - H * mpct[m] / 100 };
    lv_line_set_points(s_set.curve, s_set.curve_pts, 3);
    static const char *const mname[3] = { "ECO", "NORMAL", "SPORT" };
    ui_label_set_fmt(s_set.curve_txt, "%s mode\nNo motion below %d%%\nFull stick = %d%% power",
                     mname[m], dz, mpct[m]);
    s_set.ctl_mode_drawn = m;
    s_set.ctl_dz_drawn = dz;
}

static void radio_refresh(const scr_state_t *st)
{
    static const char *const qtxt[6] = { "NO SIGNAL", "EXCELLENT", "GOOD", "FAIR", "WEAK", "CRITICAL" };
    int lit = 0;
    uint32_t qc = UI_COL_DIM;
    if (st->rssi != 0) {
        lit = st->quality == SCR_QUAL_EXCELLENT ? 5 : st->quality == SCR_QUAL_GOOD ? 4 :
              st->quality == SCR_QUAL_FAIR ? 3 : st->quality == SCR_QUAL_WEAK ? 2 : 1;
        qc = st->quality >= SCR_QUAL_GOOD ? UI_COL_OK :
             (st->quality == SCR_QUAL_CRITICAL ? UI_COL_CRIT : UI_COL_WARN);
        ui_label_set_fmt(s_set.rd_rssi, "%d", st->rssi);
        ui_label_set_fmt(s_set.rd_ch, "%u", st->channel);
    } else {
        ui_label_set_text(s_set.rd_rssi, "--");
        ui_label_set_text(s_set.rd_ch, "--");
    }
    for (int i = 0; i < 5; i++) {
        lv_color_t c = lv_color_hex(i < lit ? qc : UI_COL_SURFACE2);
        if (!lv_color_eq(lv_obj_get_style_bg_color(s_set.rd_bar[i], 0), c)) {
            lv_obj_set_style_bg_color(s_set.rd_bar[i], c, 0);
        }
    }
    ui_label_set_text(s_set.rd_q, qtxt[(st->rssi && st->quality <= 5) ? st->quality : 0]);
    ui_label_set_color(s_set.rd_q, lv_color_hex(qc));
    scr_settings_t set;
    scr_settings_get(&set);
    ui_label_set_text(s_set.rd_ssid, set.ssid[0] ? set.ssid : "not set");
    if (st->conn == SCR_CONN_CONNECTED) {
        ui_label_set_fmt(s_set.rd_lat, "%u ms", st->lat_ms);
        ui_label_set_fmt(s_set.rd_loss, "%u.%u%%", st->loss_pct_x10 / 10, st->loss_pct_x10 % 10);
        ui_label_set_fmt(s_set.rd_rate, "%u / %u /s", st->tx_rate, st->rx_rate);
    } else {
        ui_label_set_text(s_set.rd_lat, "--");
        ui_label_set_text(s_set.rd_loss, "--");
        ui_label_set_text(s_set.rd_rate, "--");
    }
}

/* Normalise any firmware version text to "vX.Y.Z": skips a prefix such as
 * "APPFW tc275_car v", drops a git-describe tail ("-31-gc7e7581-dirty").
 * Missing minor/patch read as 0.  false (out = "--") when no number found. */
static bool fmt_semver(const char *src, char *out, size_t cap)
{
    unsigned n[3] = { 0, 0, 0 };
    int k = 0;
    const char *p = NULL;
    /* prefer a "v<digit>" token: "APPFW tc275_car v0.2.2" has digits in its
     * name ("tc275") before the version */
    for (const char *q = src; q && *q; q++) {
        if ((*q == 'v' || *q == 'V') && q[1] >= '0' && q[1] <= '9') {
            p = q + 1;
            break;
        }
    }
    if (p == NULL) {
        p = src;
        while (p && *p && (*p < '0' || *p > '9')) {
            p++;
        }
    }
    if (p == NULL || *p == '\0') {
        snprintf(out, cap, "--");
        return false;
    }
    while (k < 3 && *p >= '0' && *p <= '9') {
        unsigned x = 0;
        while (*p >= '0' && *p <= '9') {
            x = x * 10u + (unsigned)(*p++ - '0');
        }
        n[k++] = x;
        if (*p != '.') {
            break;
        }
        p++;
    }
    snprintf(out, cap, "v%u.%u.%u", n[0], n[1], n[2]);
    return true;
}

static void about_refresh(const scr_state_t *st)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char v[32];                 /* "APP  " + up to 23 B of tv */
    const char *ov;
    uint32_t oc;
    /* every node shows the same "vX.Y.Z" format */
    fmt_semver(app->version, v, sizeof(v));
    oc = UI_COL_OK;
    ov = node_ver_status(0, st, &oc);
    node_set(0, v, ov ? ov : (st->ctrl_role ? "ONLINE  CTRL" : "ONLINE"), oc);
    bool ws = st->conn == SCR_CONN_CONNECTED;
    oc = ws ? UI_COL_OK : UI_COL_CRIT;
    ov = node_ver_status(1, st, &oc);
    fmt_semver(st->c6_fw, v, sizeof(v));
    node_set(1, v, ov ? ov : (ws ? "ONLINE" : "UNREACHABLE"), oc);
    /* TC275: APP row - the version beacon string wins over the telemetry
     * fw_ver word; SBL row - beacon only ("" = SBL absent / no beacon yet) */
    char tv[24];
    bool have_tcs = st->tc_app_ver[0] && fmt_semver(st->tc_app_ver, tv, sizeof(tv));
    if (!have_tcs && st->tele_fresh) {
        snprintf(tv, sizeof(tv), "v%u.%u.%u", (unsigned)((st->tc_fw_ver >> 16) & 0xFF),
                 (unsigned)((st->tc_fw_ver >> 8) & 0xFF), (unsigned)(st->tc_fw_ver & 0xFF));
        have_tcs = true;
    }
    snprintf(v, sizeof(v), "APP  %s", have_tcs ? tv : "--");
    if (st->tele_fresh) {
        oc = st->fault_code ? UI_COL_CRIT : UI_COL_OK;
        ov = node_ver_status(2, st, &oc);
        node_set(2, v, ov ? ov : (st->fault_code ? "FAULT" : "ONLINE"), oc);
    } else {
        oc = st->tc_on ? UI_COL_WARN : UI_COL_DIM;
        ov = node_ver_status(2, st, &oc);
        node_set(2, v, ov ? ov : (st->tc_on ? "STALE" : "OFFLINE"), oc);
    }
    if (st->tc_sbl_ver[0] && fmt_semver(st->tc_sbl_ver, tv, sizeof(tv))) {
        ui_label_set_fmt(s_set.ab_sbl, "SBL  %s", tv);
    } else {
        ui_label_set_text(s_set.ab_sbl, "SBL  --");
    }
    wire_set(0, ws, ws ? "LIVE" : "DOWN");
    wire_set(1, ws && st->tc_on, ws && st->tc_on ? "LIVE" : "DOWN");

    uint32_t up = st->uptime_ms / 1000u;
    ui_label_set_fmt(s_set.ab_up, "%02lu:%02lu:%02lu", (unsigned long)(up / 3600u),
                     (unsigned long)(up / 60u % 60u), (unsigned long)(up % 60u));
    ui_label_set_fmt(s_set.ab_heap, "%u KB",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024u));
    ui_label_set_fmt(s_set.ab_psram, "%u MB",
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / (1024u * 1024u)));
    if (ws) {
        ui_label_set_fmt(s_set.ab_rtt, "%u ms", st->lat_ms);
    } else {
        ui_label_set_text(s_set.ab_rtt, "--");
    }
}

void ui_pages_settings_refresh(const scr_state_t *st)
{
    /* live values only; user-editable widgets are populated once on open
     * (sub_sync_values) so a 10 Hz repaint can never fight the keyboard */
    if (s_set.cur == SUB_NONE) {
        deck_refresh(st);
    } else if (s_set.cur == SUB_ABOUT) {
        about_refresh(st);
    }
    if (s_set.cur == SUB_CONTROL) {
        control_refresh();
    }
    if (s_set.cur == SUB_RADIO) {
        radio_refresh(st);
        ui_label_set_text(s_set.pair_status_lbl,
                          st->pair_status[0] ? st->pair_status : " ");
    }
}
