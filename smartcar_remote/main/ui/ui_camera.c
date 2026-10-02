#include <stdio.h>
#include <string.h>
#include "esp_log.h"
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

        /* one line per new size: where the frame actually sits on the panel
         * (an off-panel image reads exactly like a dead stream) */
        if (w != v->shown_w || h != v->shown_h) {
            v->shown_w = w;
            v->shown_h = h;
            lv_obj_update_layout(v->img);
            lv_area_t a;
            lv_obj_get_coords(v->img, &a);
            ESP_LOGI("ui_cam", "video %ux%u at (%d,%d)-(%d,%d)",
                     w, h, (int)a.x1, (int)a.y1, (int)a.x2, (int)a.y2);
        }
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
        /* foreground only on the hidden->visible edge: the 33 ms video pump
         * reaches here with an unchanged mask most ticks, and a redundant
         * move_foreground would churn the draw order */
        if (lv_obj_has_flag(v->mask, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_remove_flag(v->mask, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(v->mask);
        }
    } else if (!lv_obj_has_flag(v->mask, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(v->mask, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- CAMERA page ----------------------------------------------------------------
 * 800x480 budget (design doc 8.2): left 640 px video column, right 160 px control
 * sidebar.  The stream is FIXED at REMOTE_PREVIEW 320x240 (clarity stays with
 * the phone's /stream); a FULLSCREEN toggle upscales it 2x with LVGL to fill
 * the 640x480 column (strip hides, tabs overlay the bottom) - same ~11 fps and
 * same airtime, slightly soft.  No PHOTO/REC/SD widgets: the S3-CAM has no
 * MicroSD, storage features dropped. */

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
    lv_obj_t *full;
    lv_obj_t *to_vision;
    lv_obj_t *sensor;
    lv_obj_t *reason;
    ui_video_view_t view;
    bool zoom2x;                /* fullscreen toggle: LVGL 2x upscale       */
    bool immersive_applied;
} cam_ui_t;

static cam_ui_t s_cu;
static int s_panel_w, s_panel_h;

/* ---- sidebar controls ----------------------------------------------------------*/
/* One reason string serves both the sidebar row and the tap-time toast (design
 * doc 8.2), so they can never disagree.  "" = actionable.  FULLSCREEN is local
 * (LVGL scaling) and works regardless of link state - no gate on it. */
static const char *cam_block_why(const scr_state_t *st)
{
    if (st->cam.conn != SCR_CAM_CONNECTED) {
        return "CAM LINK DOWN";
    }
    if (!st->ctrl_role) {
        return "NEED CONTROL";
    }
    if (st->conn != SCR_CONN_CONNECTED) {
        return "LINK NOT READY";
    }
    return "";
}

/* 2x nearest-neighbour upscale around the image centre: 320x240 -> exactly the
 * 640x480 video column.  Antialiasing stays off - cheaper and the softness is
 * the accepted trade for full-rate QVGA airtime. */
static void cam_apply_zoom(void)
{
    if (s_cu.zoom2x) {
        lv_image_set_scale(s_cu.view.img, 2u * LV_SCALE_NONE);
        lv_image_set_pivot(s_cu.view.img, 160, 120);   /* QVGA centre */
    } else {
        lv_image_set_scale(s_cu.view.img, LV_SCALE_NONE);
        lv_image_set_pivot(s_cu.view.img, 0, 0);
    }
    lv_image_set_antialias(s_cu.view.img, false);
    lv_obj_center(s_cu.view.img);
    lv_obj_t *lbl = lv_obj_get_child(s_cu.full, 0);
    ui_label_set_text(lbl, s_cu.zoom2x ? LV_SYMBOL_MINUS " WINDOW 1x"
                                       : LV_SYMBOL_PLUS " FULLSCREEN 2x");
    if (s_cu.zoom2x) {
        lv_obj_add_state(s_cu.full, LV_STATE_CHECKED);
    } else {
        lv_obj_remove_state(s_cu.full, LV_STATE_CHECKED);
    }
}

static void full_cb(lv_event_t *e)
{
    (void)e;
    s_cu.zoom2x = !s_cu.zoom2x;
    cam_apply_zoom();
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

/* Strip fields are fixed width and DOTS-truncated (the DRIVE top bar uses the
 * same discipline): free-width labels on a fixed pitch let the longest string
 * win - "✕ CAM OFF" at ~90 px grew across the 74 px gap into "-- fps", which is
 * the overlapping text in the corner. */
static lv_obj_t *strip_field(lv_obj_t *parent, const char *cap, int x, int w)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, cap);
    lv_obj_set_style_text_font(l, F_SM, 0);
    lv_obj_set_size(l, w, lv_font_get_line_height(F_SM));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, x, 0);
    return l;
}

/* Width of `s` in F_SM, measured by the layout engine itself: hand-guessed
 * pixel budgets are how "✕ CAM OFF" got clipped in the first place. */
static int32_t strip_text_w(lv_obj_t *parent, const char *s)
{
    lv_obj_t *m = lv_label_create(parent);
    lv_obj_set_style_text_font(m, F_SM, 0);
    lv_label_set_text(m, s);
    lv_obj_update_layout(m);
    int32_t w = lv_obj_get_width(m);
    lv_obj_delete(m);
    return w;
}

void ui_camera_create(lv_obj_t *root)
{
    s_panel_w = bsp_display_get_h_res();
    s_panel_h = bsp_display_get_v_res();
    /* opposite of the apply_layout(false) that ends this function: with
     * "false" here that first call hit the no-change early return, the
     * video area was never sized/placed and the frame landed off-panel
     * (bench 10-02: img at (-95,-55)-(224,184), page all black) */
    s_cu.immersive_applied = true;

    /* top info strip */
    s_cu.strip = lv_obj_create(root);
    lv_obj_set_size(s_cu.strip, s_panel_w, STRIP_H);
    lv_obj_align(s_cu.strip, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_cu.strip, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_cu.strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cu.strip, 0, 0);
    lv_obj_set_style_pad_all(s_cu.strip, 0, 0);
    lv_obj_remove_flag(s_cu.strip, LV_OBJ_FLAG_SCROLLABLE);

    /* Five columns sized from the real strings, not from a hand-tuned table
     * (the fixed 110 px first column clipped "✕ CAM OFF" - the top-left field
     * this page is read by).  Every bounded field gets exactly its widest
     * text; only the unbounded drop counter keeps DOTS as a floor.  All
     * columns stay inside the 640 px video column so the sidebar never clips
     * a value. */
    static const char *const DOT_MAX[] = {
        LV_SYMBOL_CLOSE " CAM OFF", LV_SYMBOL_PLAY " CAM LINK",
        LV_SYMBOL_WARNING " STALE", LV_SYMBOL_OK " LIVE",
        LV_SYMBOL_EYE_OPEN " IDLE",
    };
    int32_t dot_w = 0;
    for (size_t i = 0; i < sizeof(DOT_MAX) / sizeof(DOT_MAX[0]); i++) {
        int32_t w = strip_text_w(s_cu.strip, DOT_MAX[i]);
        if (w > dot_w) {
            dot_w = w;
        }
    }
    const int GAP = 14;
    const int X0 = 8;
    const int RIGHT = s_panel_w - SIDE_W - 8;    /* video column right edge */
    int x = X0;
    s_cu.dot      = strip_field(s_cu.strip, DOT_MAX[0], x, (int)dot_w);
    x += (int)dot_w + GAP;
    int fps_w = strip_text_w(s_cu.strip, "99.9 fps");
    s_cu.fps      = strip_field(s_cu.strip, "-- fps", x, fps_w);
    x += fps_w + GAP;
    int res_w = strip_text_w(s_cu.strip, "640x480");
    s_cu.res      = strip_field(s_cu.strip, "-x-", x, res_w);
    x += res_w + GAP;
    int e2e_w = strip_text_w(s_cu.strip, "e2e 65535 ms");
    int drop_min = strip_text_w(s_cu.strip, "drop 99999/99999");
    int drop_w = (RIGHT - e2e_w - GAP) - x;
    if (drop_w < drop_min) {
        drop_w = drop_min;
    }
    s_cu.drop_lbl = strip_field(s_cu.strip, "drop -", x, drop_w);
    s_cu.e2e      = strip_field(s_cu.strip, "e2e -", RIGHT - e2e_w, e2e_w);

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
    /* fullscreen toggle, radio-like checked styling (matches the tab bar):
     * stream size itself is FIXED 320x240 - clarity stays with the phone's
     * /stream, the handset only ever decides how big it is drawn */
    s_cu.full = side_button(s_cu.side, LV_SYMBOL_PLUS " FULLSCREEN 2x", 30, 30,
                            full_cb, NULL);
    lv_obj_set_style_bg_color(s_cu.full, lv_color_hex(0x0B2A3A), LV_STATE_CHECKED);
    lv_obj_set_style_border_color(s_cu.full, lv_color_hex(UI_COL_ACCENT), LV_STATE_CHECKED);
    lv_obj_set_style_border_width(s_cu.full, 2, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(lv_obj_get_child(s_cu.full, 0),
                                lv_color_hex(UI_COL_ACCENT), LV_STATE_CHECKED);
    s_cu.to_vision = side_button(s_cu.side, "VISION PAGE", 64, 30, to_vision_cb, NULL);
    s_cu.reason = side_label(s_cu.side, "", 104);
    ui_label_set_color(s_cu.reason, lv_color_hex(UI_COL_WARN));

    s_cu.zoom2x = false;
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

    /* sidebar rows */
    set_side(s_cu.sensor, "CAM",
             st->cam.sensor[0] != '\0' ? st->cam.sensor : "-",
             lv_color_hex(UI_COL_TXT));

    /* LINK state dims the VISION hand-off only; FULLSCREEN is local scaling
     * and stays usable regardless (design doc 8.2 gate discipline) */
    const char *why = cam_block_why(st);
    ui_set_blocked(s_cu.to_vision, why[0] != '\0');
    ui_label_set_text(s_cu.reason, why);

    apply_layout(s_cu.zoom2x || ui_video_immersive());
    ui_video_view_pump(&s_cu.view, st);
}

/* Fast-cadence video pump, driven by the 33 ms timer in ui.c: a decoded frame
 * reaches the panel without waiting for the 100 ms page refresh.  The seq gate
 * in ui_video_view_pump makes every tick without a new frame a no-op. */
void ui_camera_pump_video(const scr_state_t *st)
{
    ui_video_view_pump(&s_cu.view, st);
}
