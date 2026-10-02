#include <stdio.h>
#include <string.h>
#include <bsp/esp-bsp.h>

#include "ui_camera.h"
#include "ui_theme.h"
#include "ui.h"

#include "../app_state.h"
#include "../scr_cam.h"
#include "../scr_link.h"
#include "../proto/vision.h"

/* ---- shared video view -------------------------------------------------------
 * One 1:1 RGB565 image centred in a black area + a NO SIGNAL mask.  Frames
 * arrive through scr_cam's slot ring; a changed SEQ is the ONLY repaint source
 * (design doc 5.3: seq unchanged -> no invalidate). */

/* full-bleed flag: a stream taller than the normal video area (WEB_PREVIEW
 * 640x480) forces the immersive layout on both camera-facing pages. */
static bool s_immersive;

bool ui_video_immersive(void)
{
    return s_immersive;
}

void ui_video_view_create(ui_video_view_t *v, lv_obj_t *parent)
{
    v->area = parent;
    v->cur_seq = 0;
    v->has_frame = false;

    lv_obj_set_style_bg_color(parent, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(parent, 1, 0);
    lv_obj_set_style_border_color(parent, lv_color_hex(UI_COL_LINE), 0);
    lv_obj_set_style_pad_all(parent, 0, 0);

    v->img = lv_image_create(parent);
    lv_obj_center(v->img);
    lv_obj_remove_flag(v->img, LV_OBJ_FLAG_CLICKABLE);

    v->mask = lv_label_create(parent);
    lv_label_set_text(v->mask, "NO SIGNAL");
    lv_obj_set_style_text_font(v->mask, F_LG, 0);
    lv_obj_set_style_bg_color(v->mask, lv_color_hex(UI_COL_BG), 0);
    lv_obj_set_style_bg_opa(v->mask, LV_OPA_70, 0);
    lv_obj_set_style_radius(v->mask, 8, 0);
    lv_obj_set_style_pad_all(v->mask, 10, 0);
    lv_obj_center(v->mask);
}

void ui_video_view_pump(ui_video_view_t *v, const scr_state_t *st)
{
    uint16_t w = 0, h = 0;
    uint32_t seq = 0;
    const uint8_t *buf = scr_cam_display_acquire(&w, &h, &seq);
    if (buf != NULL && seq != v->cur_seq) {
        v->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        v->dsc.header.w = w;
        v->dsc.header.h = h;
        v->dsc.data = buf;
        v->dsc.data_size = (uint32_t)w * h * 2u;
        lv_image_set_src(v->img, &v->dsc);
        lv_obj_center(v->img);
        v->cur_seq = seq;
        v->has_frame = true;
        s_immersive = (h > 400);
    }

    /* mask = every non-live condition, with text saying which one (spec 51:
     * never colour alone).  The last frame stays visible underneath. */
    const char *txt;
    lv_color_t col;
    bool show;
    if (st->cam.conn != SCR_CAM_CONNECTED) {
        show = true;
        if (st->cam.conn == SCR_CAM_CONNECTING) {
            txt = "CAM CONNECTING"; col = lv_color_hex(UI_COL_INFO);
        } else {
            txt = "CAM OFFLINE";    col = lv_color_hex(UI_COL_CRIT);
        }
    } else if (!st->cam.subscribed) {
        show = true; txt = "PREVIEW PAUSED"; col = lv_color_hex(UI_COL_DIM);
    } else if (st->cam.stale || !v->has_frame) {
        show = true; txt = "NO SIGNAL";      col = lv_color_hex(UI_COL_WARN);
    } else {
        show = false; txt = NULL; col = lv_color_hex(UI_COL_DIM);
    }
    if (show) {
        ui_label_set_text(v->mask, txt);
        ui_label_set_color(v->mask, col);
        lv_obj_remove_flag(v->mask, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(v->mask);
    } else {
        lv_obj_add_flag(v->mask, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- CAMERA page ----------------------------------------------------------------
 * 800x480 budget (design doc 8.2): left 640 px video column, right 160 px control
 * sidebar.  REMOTE_PREVIEW 320x240 shows centred with the 36 px info strip and
 * the tab bar; WEB_PREVIEW 640x480 goes immersive (strip + tabs hide).
 * No PHOTO/REC/SD widgets: the S3-CAM has no MicroSD, storage features dropped. */

#define SIDE_W    160
#define STRIP_H   36
#define TAB_H     44

typedef struct {
    lv_obj_t *strip;
    lv_obj_t *area;
    lv_obj_t *side;
    lv_obj_t *dot;
    lv_obj_t *fps;
    lv_obj_t *res;
    lv_obj_t *drop_lbl;
    lv_obj_t *e2e;
    lv_obj_t *prof[2];
    lv_obj_t *to_vision;
    lv_obj_t *sensor;
    lv_obj_t *reason;
    ui_video_view_t view;
    bool immersive_applied;
} cam_ui_t;

static cam_ui_t s_cu;
static int s_panel_w, s_panel_h;

/* ---- sidebar controls ----------------------------------------------------------*/
static bool cam_ctrl_gate(void)
{
    scr_state_t st;
    app_state_snapshot(&st);
    return st.ctrl_role && st.conn == SCR_CONN_CONNECTED &&
           st.cam.conn == SCR_CAM_CONNECTED;
}

static void prof_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (!cam_ctrl_gate()) {
        ui_toast("CAM: CONTROL NEEDED");
        return;
    }
    scr_cam_cmd_profile(idx == 0 ? CAM_PROFILE_REMOTE : CAM_PROFILE_WEB);
}

static void to_vision_cb(lv_event_t *e)
{
    (void)e;
    ui_nav_open(UI_PAGE_VISION);
}

static lv_obj_t *side_label(lv_obj_t *parent, const char *cap, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, cap);
    lv_obj_set_style_text_font(l, F_SM, 0);
    lv_obj_set_size(l, SIDE_W - 16, lv_font_get_line_height(F_SM));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 8, y);
    return l;
}

static lv_obj_t *side_button(lv_obj_t *parent, const char *txt, int y, int h,
                             lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = ui_button(parent, txt, lv_color_hex(UI_COL_SURFACE2), cb, ud);
    lv_obj_set_size(b, SIDE_W - 16, h);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 8, y);
    return b;
}

/* one-line key/value text in the sidebar */
static void set_side(lv_obj_t *lbl, const char *cap, const char *val, lv_color_t col)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%s %s", cap, val);
    ui_label_set_text(lbl, buf);
    ui_label_set_color(lbl, col);
}

static void apply_layout(bool immersive)
{
    if (immersive == s_cu.immersive_applied) {
        return;
    }
    s_cu.immersive_applied = immersive;
    if (immersive) {
        lv_obj_add_flag(s_cu.strip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_cu.area, s_panel_w - SIDE_W, s_panel_h);
        lv_obj_align(s_cu.area, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_size(s_cu.side, SIDE_W, s_panel_h);
        lv_obj_align(s_cu.side, LV_ALIGN_TOP_RIGHT, 0, 0);
    } else {
        lv_obj_remove_flag(s_cu.strip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_cu.area, s_panel_w - SIDE_W, s_panel_h - STRIP_H - TAB_H);
        lv_obj_align(s_cu.area, LV_ALIGN_TOP_LEFT, 0, STRIP_H);
        lv_obj_set_size(s_cu.side, SIDE_W, s_panel_h - STRIP_H);
        lv_obj_align(s_cu.side, LV_ALIGN_TOP_RIGHT, 0, STRIP_H);
    }
}

void ui_camera_create(lv_obj_t *root)
{
    s_panel_w = bsp_display_get_h_res();
    s_panel_h = bsp_display_get_v_res();
    s_cu.immersive_applied = false;

    /* top info strip */
    s_cu.strip = lv_obj_create(root);
    lv_obj_set_size(s_cu.strip, s_panel_w, STRIP_H);
    lv_obj_align(s_cu.strip, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_cu.strip, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_cu.strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cu.strip, 0, 0);
    lv_obj_set_style_pad_all(s_cu.strip, 0, 0);
    lv_obj_remove_flag(s_cu.strip, LV_OBJ_FLAG_SCROLLABLE);

    const int SX[5] = { 18, 92, 180, 262, 430 };
    s_cu.dot = lv_label_create(s_cu.strip);
    lv_label_set_text(s_cu.dot, LV_SYMBOL_CLOSE " CAM");
    lv_obj_set_style_text_font(s_cu.dot, F_SM, 0);
    ui_label_set_color(s_cu.dot, lv_color_hex(UI_COL_DIM));
    lv_obj_align(s_cu.dot, LV_ALIGN_LEFT_MID, SX[0], 0);
    s_cu.fps = lv_label_create(s_cu.strip);
    lv_label_set_text(s_cu.fps, "-- fps");
    lv_obj_set_style_text_font(s_cu.fps, F_SM, 0);
    lv_obj_align(s_cu.fps, LV_ALIGN_LEFT_MID, SX[1], 0);
    s_cu.res = lv_label_create(s_cu.strip);
    lv_label_set_text(s_cu.res, "-x-");
    lv_obj_set_style_text_font(s_cu.res, F_SM, 0);
    lv_obj_align(s_cu.res, LV_ALIGN_LEFT_MID, SX[2], 0);
    s_cu.drop_lbl = lv_label_create(s_cu.strip);
    lv_label_set_text(s_cu.drop_lbl, "drop -");
    lv_obj_set_style_text_font(s_cu.drop_lbl, F_SM, 0);
    lv_obj_align(s_cu.drop_lbl, LV_ALIGN_LEFT_MID, SX[3], 0);
    s_cu.e2e = lv_label_create(s_cu.strip);
    lv_label_set_text(s_cu.e2e, "e2e -");
    lv_obj_set_style_text_font(s_cu.e2e, F_SM, 0);
    lv_obj_align(s_cu.e2e, LV_ALIGN_LEFT_MID, SX[4], 0);

    /* video area */
    s_cu.area = lv_obj_create(root);
    ui_video_view_create(&s_cu.view, s_cu.area);

    /* sidebar */
    s_cu.side = lv_obj_create(root);
    lv_obj_set_style_bg_color(s_cu.side, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_cu.side, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cu.side, 1, 0);
    lv_obj_set_style_border_color(s_cu.side, lv_color_hex(UI_COL_LINE), 0);
    lv_obj_set_style_pad_all(s_cu.side, 0, 0);
    lv_obj_remove_flag(s_cu.side, LV_OBJ_FLAG_SCROLLABLE);

    s_cu.sensor = side_label(s_cu.side, "CAM -", 8);
    s_cu.prof[0] = side_button(s_cu.side, "320 PREVIEW", 30, 30, prof_cb, (void *)0);
    s_cu.prof[1] = side_button(s_cu.side, "640 FULL", 64, 30, prof_cb, (void *)1);
    s_cu.to_vision = side_button(s_cu.side, "VISION PAGE", 100, 30, to_vision_cb, NULL);
    s_cu.reason = side_label(s_cu.side, "", 140);
    ui_label_set_color(s_cu.reason, lv_color_hex(UI_COL_WARN));

    apply_layout(false);
}

void ui_camera_refresh(const scr_state_t *st)
{
    /* strip: dot carries state as TEXT + colour (spec 51) */
    const char *dot_txt;
    lv_color_t dot_col;
    if (st->cam.conn != SCR_CAM_CONNECTED) {
        dot_txt = (st->cam.conn == SCR_CAM_CONNECTING) ? LV_SYMBOL_PLAY " CAM LINK"
                                                      : LV_SYMBOL_CLOSE " CAM OFF";
        dot_col = (st->cam.conn == SCR_CAM_CONNECTING) ? lv_color_hex(UI_COL_INFO)
                                                       : lv_color_hex(UI_COL_CRIT);
    } else if (st->cam.stale) {
        dot_txt = LV_SYMBOL_WARNING " STALE";
        dot_col = lv_color_hex(UI_COL_WARN);
    } else if (st->cam.fps_x10 > 0) {
        dot_txt = LV_SYMBOL_OK " LIVE";
        dot_col = lv_color_hex(UI_COL_OK);
    } else {
        dot_txt = LV_SYMBOL_EYE_OPEN " IDLE";
        dot_col = lv_color_hex(UI_COL_DIM);
    }
    ui_label_set_text(s_cu.dot, dot_txt);
    ui_label_set_color(s_cu.dot, dot_col);

    if (st->cam.fps_x10 > 0) {
        ui_label_set_fmt(s_cu.fps, "%u.%u fps", st->cam.fps_x10 / 10, st->cam.fps_x10 % 10);
    } else {
        ui_label_set_text(s_cu.fps, "-- fps");
    }
    if (st->cam.w > 0) {
        ui_label_set_fmt(s_cu.res, "%ux%u", st->cam.w, st->cam.h);
    } else {
        ui_label_set_text(s_cu.res, "-x-");
    }
    ui_label_set_fmt(s_cu.drop_lbl, "drop %lu/%lu",
                     (unsigned long)st->cam.drop, (unsigned long)st->cam.seq);
    ui_label_set_fmt(s_cu.e2e, "e2e %u ms", st->cam.e2e_ms);

    /* profile radio reflects the RETURNED dims, never a local guess (spec 106) */
    int live_idx = (st->cam.w >= 640) ? 1 : 0;
    for (int i = 0; i < 2; i++) {
        if ((i == live_idx) != lv_obj_has_state(s_cu.prof[i], LV_STATE_CHECKED)) {
            if (i == live_idx) {
                lv_obj_add_state(s_cu.prof[i], LV_STATE_CHECKED);
            } else {
                lv_obj_remove_state(s_cu.prof[i], LV_STATE_CHECKED);
            }
        }
    }

    /* sidebar rows */
    set_side(s_cu.sensor, "CAM",
             st->cam.sensor[0] != '\0' ? st->cam.sensor : "-",
             lv_color_hex(UI_COL_TXT));

    /* CTRL gating: grey out + state the reason (design doc 8.2) */
    bool en = st->ctrl_role && st->conn == SCR_CONN_CONNECTED &&
              st->cam.conn == SCR_CAM_CONNECTED;
    lv_obj_t *gated[] = { s_cu.prof[0], s_cu.prof[1] };
    for (size_t i = 0; i < sizeof(gated) / sizeof(gated[0]); i++) {
        if (en) {
            lv_obj_remove_state(gated[i], LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(gated[i], LV_STATE_DISABLED);
        }
    }
    const char *why = "";
    if (!en) {
        if (st->cam.conn != SCR_CAM_CONNECTED) {
            why = "CAM LINK DOWN";
        } else if (!st->ctrl_role) {
            why = "NEED CONTROL";
        } else {
            why = "LINK NOT READY";
        }
    }
    ui_label_set_text(s_cu.reason, why);

    apply_layout(ui_video_immersive());
    ui_video_view_pump(&s_cu.view, st);
}
