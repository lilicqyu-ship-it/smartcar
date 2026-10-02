#include <stdio.h>
#include <string.h>
#include <bsp/esp-bsp.h>
#include <sdkconfig.h>

#include "ui_vision.h"
#include "ui_camera.h"
#include "ui_theme.h"
#include "ui.h"

#include "../app_state.h"
#include "../scr_link.h"
#include "../proto/vision.h"

/* ---- VISION page (design doc 8.3) ----------------------------------------------
 * Same video column as the CAMERA page (shared slot pump), plus a geometry
 * overlay drawn 1:1 on the frame (vision coords ARE preview-frame pixels,
 * contract in proto/vision.h) and a numeric + control sidebar.
 *
 * Interlock (design doc 7.3): vision.fresh == false greys the panel, hides the
 * overlay and forces the ASSIST button back to MANUAL - the remote never
 * displays a stale visual result as live (spec 101). */

#define SIDE_W    160
#define STRIP_H   36
#define TAB_H     44

typedef struct {
    lv_obj_t *strip;
    lv_obj_t *area;
    lv_obj_t *side;
    lv_obj_t *head;         /* "VISION  MODE LINE ..." */
    lv_obj_t *fresh_dot;
    lv_obj_t *err, *ang, *conf, *objs, *mode_row;
    lv_obj_t *mode_btn[VISION_MODE_IDX_COUNT];   /* OFF LINE COLOR QR OBJ  */
    lv_obj_t *drive_btn[3];                      /* MANUAL ASSIST AUTO     */
    lv_obj_t *reason;
    ui_video_view_t view;
#if CONFIG_SCR_CAM_OVERLAY
    lv_obj_t *ref_line;     /* frame centre reference, child of the image   */
    lv_obj_t *det_line;     /* detected line through cx, tilted by angle    */
    lv_point_precise_t ref_pts[2];
    lv_point_precise_t det_pts[2];
#endif
    bool immersive_applied;
} vis_ui_t;

static vis_ui_t s_vis;
static int s_panel_w, s_panel_h;

/* ---- commands (all CTRL gated, design doc 7.2) ---------------------------------*/
/* Gate reasons double as the tap-time toast and the sidebar row, so the two
 * cannot drift apart.  The buttons are gated with ui_set_blocked rather than
 * LV_STATE_DISABLED on purpose: a disabled widget never sees the click, and
 * these gates exist to tell the driver why the tap did nothing.  "" = go. */
static const char *vis_gate_why(const scr_state_t *st)
{
    if (st->conn != SCR_CONN_CONNECTED) {
        return "LINK DOWN";
    }
    if (!st->ctrl_role) {
        return "NEED CONTROL";
    }
    return "";
}

static const char *vis_drive_why(const scr_state_t *st, int idx)
{
    if (idx == 2) {
        /* LLDD Table 16: full AUTO is not part of V1.0 - placeholder button */
        return "AUTO NOT IN V1.0";
    }
    const char *why = vis_gate_why(st);
    if (why[0] != '\0') {
        return why;
    }
    if (idx == 1 && !st->vision.fresh) {
        return "VISION STALE";
    }
    return "";
}

static void send_json(const char *buf)
{
    if (!scr_link_send_text(buf)) {
        ui_toast("NOT SENT: LINK");
    }
}

static void mode_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    scr_state_t st;
    app_state_snapshot(&st);
    const char *why = vis_gate_why(&st);
    if (why[0] != '\0') {
        ui_toast("VISION: %s", why);
        return;
    }
    char buf[VISION_CMD_BUF_MAX];
    if (vision_fmt_mode_cmd(buf, sizeof(buf), vision_mode_str(idx),
                            idx != VISION_MODE_IDX_OFF) == 0) {
        return;
    }
    send_json(buf);
}

static void drive_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    scr_state_t st;
    app_state_snapshot(&st);
    const char *why = vis_drive_why(&st, idx);
    if (why[0] != '\0') {
        ui_toast("%s", why);
        return;
    }
    char buf[VISION_CMD_BUF_MAX];
    if (vision_fmt_drive_mode(buf, sizeof(buf), vision_drive_mode_str(idx)) == 0) {
        return;
    }
    send_json(buf);
}

static lv_obj_t *side_label(lv_obj_t *parent, const char *cap, int y, lv_color_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, cap);
    lv_obj_set_style_text_font(l, F_SM, 0);
    ui_label_set_color(l, col);
    lv_obj_set_size(l, SIDE_W - 16, lv_font_get_line_height(F_SM));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 8, y);
    return l;
}

static lv_obj_t *side_button(lv_obj_t *parent, const char *txt, int x, int y,
                             int w, int h, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = ui_button(parent, txt, lv_color_hex(UI_COL_SURFACE2), cb, ud);
    lv_obj_set_size(b, w, h);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, x, y);
    /* the label inside must react to CHECKED/DISABLED with the button */
    lv_obj_add_flag(lv_obj_get_child(b, 0), LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_text_color(lv_obj_get_child(b, 0), lv_color_hex(UI_COL_ACCENT),
                                LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0B2A3A), LV_STATE_CHECKED);
    lv_obj_set_style_border_color(b, lv_color_hex(UI_COL_ACCENT), LV_STATE_CHECKED);
    lv_obj_set_style_border_width(b, 2, LV_STATE_CHECKED);
    return b;
}

static void apply_layout(bool immersive)
{
    if (immersive == s_vis.immersive_applied) {
        return;
    }
    s_vis.immersive_applied = immersive;
    if (immersive) {
        lv_obj_add_flag(s_vis.strip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_vis.area, s_panel_w - SIDE_W, s_panel_h);
        lv_obj_align(s_vis.area, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_set_size(s_vis.side, SIDE_W, s_panel_h);
        lv_obj_align(s_vis.side, LV_ALIGN_TOP_RIGHT, 0, 0);
    } else {
        lv_obj_remove_flag(s_vis.strip, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_vis.area, s_panel_w - SIDE_W, s_panel_h - STRIP_H - TAB_H);
        lv_obj_align(s_vis.area, LV_ALIGN_TOP_LEFT, 0, STRIP_H);
        lv_obj_set_size(s_vis.side, SIDE_W, s_panel_h - STRIP_H);
        lv_obj_align(s_vis.side, LV_ALIGN_TOP_RIGHT, 0, STRIP_H);
    }
}

void ui_vision_create(lv_obj_t *root)
{
    s_panel_w = bsp_display_get_h_res();
    s_panel_h = bsp_display_get_v_res();
    s_vis.immersive_applied = false;

    s_vis.strip = lv_obj_create(root);
    lv_obj_set_size(s_vis.strip, s_panel_w, STRIP_H);
    lv_obj_align(s_vis.strip, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_vis.strip, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_vis.strip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_vis.strip, 0, 0);
    lv_obj_set_style_pad_all(s_vis.strip, 0, 0);
    lv_obj_remove_flag(s_vis.strip, LV_OBJ_FLAG_SCROLLABLE);
    s_vis.head = lv_label_create(s_vis.strip);
    lv_label_set_text(s_vis.head, "VISION  OFF");
    lv_obj_set_style_text_font(s_vis.head, F_SM, 0);
    lv_obj_align(s_vis.head, LV_ALIGN_LEFT_MID, 18, 0);
    s_vis.fresh_dot = lv_label_create(s_vis.strip);
    lv_label_set_text(s_vis.fresh_dot, "-");
    lv_obj_set_style_text_font(s_vis.fresh_dot, F_SM, 0);
    ui_label_set_color(s_vis.fresh_dot, lv_color_hex(UI_COL_DIM));
    lv_obj_align(s_vis.fresh_dot, LV_ALIGN_RIGHT_MID, -12, 0);

    s_vis.area = lv_obj_create(root);
    ui_video_view_create(&s_vis.view, s_vis.area);

#if CONFIG_SCR_CAM_OVERLAY
    /* overlay lines are children of the image: 1:1 display means frame pixels
     * map directly, and LVGL clips them to the image bounds for free */
    s_vis.ref_line = lv_line_create(s_vis.view.img);
    s_vis.ref_pts[0].x = 0;   s_vis.ref_pts[0].y = 0;
    s_vis.ref_pts[1].x = 0;   s_vis.ref_pts[1].y = 0;
    lv_line_set_points(s_vis.ref_line, s_vis.ref_pts, 2);
    lv_obj_set_style_line_color(s_vis.ref_line, lv_color_hex(UI_COL_DIM), 0);
    lv_obj_set_style_line_width(s_vis.ref_line, 1, 0);
    lv_obj_set_style_line_dash_width(s_vis.ref_line, 4, 0);
    lv_obj_set_style_line_dash_gap(s_vis.ref_line, 4, 0);
    lv_obj_remove_flag(s_vis.ref_line, LV_OBJ_FLAG_CLICKABLE);

    s_vis.det_line = lv_line_create(s_vis.view.img);
    s_vis.det_pts[0].x = 0;   s_vis.det_pts[0].y = 0;
    s_vis.det_pts[1].x = 0;   s_vis.det_pts[1].y = 0;
    lv_line_set_points(s_vis.det_line, s_vis.det_pts, 2);
    lv_obj_set_style_line_color(s_vis.det_line, lv_color_hex(UI_COL_OK), 0);
    lv_obj_set_style_line_width(s_vis.det_line, 2, 0);
    lv_obj_remove_flag(s_vis.det_line, LV_OBJ_FLAG_CLICKABLE);
#endif

    s_vis.side = lv_obj_create(root);
    lv_obj_set_style_bg_color(s_vis.side, lv_color_hex(UI_COL_SURFACE), 0);
    lv_obj_set_style_bg_opa(s_vis.side, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_vis.side, 1, 0);
    lv_obj_set_style_border_color(s_vis.side, lv_color_hex(UI_COL_LINE), 0);
    lv_obj_set_style_pad_all(s_vis.side, 0, 0);
    lv_obj_remove_flag(s_vis.side, LV_OBJ_FLAG_SCROLLABLE);

    /* numeric panel */
    s_vis.err   = side_label(s_vis.side, "ERROR --", 8, lv_color_hex(UI_COL_TXT));
    s_vis.ang   = side_label(s_vis.side, "ANGLE --", 28, lv_color_hex(UI_COL_TXT));
    s_vis.conf  = side_label(s_vis.side, "CONF --", 48, lv_color_hex(UI_COL_TXT));
    s_vis.objs  = side_label(s_vis.side, "OBJ --", 68, lv_color_hex(UI_COL_TXT));
    s_vis.mode_row = side_label(s_vis.side, "ASSIST OFF", 88, lv_color_hex(UI_COL_TXT));

    /* mode buttons: OFF|LINE / COLOR|QR / OBJ  (72 px wide, 4 px gutter) */
    static const char *mtxt[VISION_MODE_IDX_COUNT] = {
        "OFF", "LINE", "COLOR", "QR", "OBJ"
    };
    const int mx[VISION_MODE_IDX_COUNT] = { 8, 84, 8, 84, 8 };
    const int my[VISION_MODE_IDX_COUNT] = { 112, 112, 146, 146, 180 };
    for (int i = 0; i < VISION_MODE_IDX_COUNT; i++) {
        s_vis.mode_btn[i] = side_button(s_vis.side, mtxt[i], mx[i], my[i], 72, 30,
                                        mode_cb, (void *)(uintptr_t)i);
    }
    /* drive mode: one per row (MANUAL/ASSIST/AUTO) */
    static const char *dtxt[3] = { "MANUAL", "ASSIST", "AUTO" };
    for (int i = 0; i < 3; i++) {
        s_vis.drive_btn[i] = side_button(s_vis.side, dtxt[i], 8, 216 + i * 34,
                                         SIDE_W - 16, 30, drive_cb,
                                         (void *)(uintptr_t)i);
    }
    s_vis.reason = side_label(s_vis.side, "", 322, lv_color_hex(UI_COL_WARN));

    apply_layout(false);
}

static void set_checked(lv_obj_t *btn, bool on)
{
    if (on != lv_obj_has_state(btn, LV_STATE_CHECKED)) {
        if (on) {
            lv_obj_add_state(btn, LV_STATE_CHECKED);
        } else {
            lv_obj_remove_state(btn, LV_STATE_CHECKED);
        }
    }
}

static void set_enabled(lv_obj_t *btn, bool en)
{
    ui_set_blocked(btn, !en);
}

#if CONFIG_SCR_CAM_OVERLAY
static void overlay_update(const scr_state_t *st)
{
    /* move the image's children with the frame: only meaningful while the
     * LINE result is fresh AND the preview is actually showing frames */
    bool show = st->vision.fresh && st->cam.subscribed && !st->cam.stale &&
                s_vis.view.has_frame &&
                st->vision.mode == VISION_MODE_IDX_LINE;
    if (!show) {
        lv_obj_add_flag(s_vis.ref_line, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_vis.det_line, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    uint16_t w = s_vis.view.dsc.header.w;
    uint16_t h = s_vis.view.dsc.header.h;
    if (w == 0 || h == 0) {
        return;
    }
    /* keep both lines centred on the image: children share the image coords,
     * the image itself sits at a negative offset inside the area when smaller
     * than the area, which LVGL applies to children automatically */
    s_vis.ref_pts[0].x = w / 2;   s_vis.ref_pts[0].y = 0;
    s_vis.ref_pts[1].x = w / 2;   s_vis.ref_pts[1].y = h;
    int cx = st->vision.cx;
    /* tilt: dx across half the frame = tan(angle) * h/2, tan ~ angle in rad
     * (|angle| < ~12 deg in practice); angle_x10 degrees x10 */
    int32_t dxh = (int32_t)st->vision.angle_x10 * h / 2290;  /* /2290 ~ rad/2 */
    s_vis.det_pts[0].x = (lv_coord_t)(cx - dxh);
    s_vis.det_pts[0].y = 0;
    s_vis.det_pts[1].x = (lv_coord_t)(cx + dxh);
    s_vis.det_pts[1].y = h;
    lv_line_set_points(s_vis.ref_line, s_vis.ref_pts, 2);
    lv_line_set_points(s_vis.det_line, s_vis.det_pts, 2);
    lv_obj_remove_flag(s_vis.ref_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_vis.det_line, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_vis.det_line);
    lv_obj_move_foreground(s_vis.view.mask);   /* mask always on top */
}
#endif

void ui_vision_refresh(const scr_state_t *st)
{
    bool fresh = st->vision.fresh;

    /* header strip */
    ui_label_set_fmt(s_vis.head, "VISION  %s",
                     st->vision.mode < VISION_MODE_IDX_COUNT
                         ? vision_mode_str(st->vision.mode) : "?");
    const char *dot_txt;
    lv_color_t dot_col;
    if (st->vision.mode == VISION_MODE_IDX_OFF) {
        dot_txt = "OFF";    dot_col = lv_color_hex(UI_COL_DIM);
    } else if (!fresh) {
        dot_txt = "STALE";  dot_col = lv_color_hex(UI_COL_WARN);
    } else if (!st->vision.valid) {
        dot_txt = "NO LOCK"; dot_col = lv_color_hex(UI_COL_WARN);
    } else {
        dot_txt = "LIVE";   dot_col = lv_color_hex(UI_COL_OK);
    }
    ui_label_set_text(s_vis.fresh_dot, dot_txt);
    ui_label_set_color(s_vis.fresh_dot, dot_col);

    /* numeric panel: stale shows STALE, never the last live value (spec 101) */
    if (st->vision.mode == VISION_MODE_IDX_OFF) {
        ui_label_set_text(s_vis.err, "ERROR --");
        ui_label_set_text(s_vis.ang, "ANGLE --");
        ui_label_set_text(s_vis.conf, "CONF --");
        ui_label_set_text(s_vis.objs, "OBJ --");
    } else if (!fresh || !st->vision.valid) {
        ui_label_set_text(s_vis.err, "ERROR STALE");
        ui_label_set_text(s_vis.ang, "ANGLE STALE");
        ui_label_set_text(s_vis.conf, "CONF STALE");
        if (st->vision.mode == VISION_MODE_IDX_OBJECT) {
            ui_label_set_fmt(s_vis.objs, "OBJ %u", st->vision.objects_count);
        } else {
            ui_label_set_text(s_vis.objs, "OBJ --");
        }
    } else {
        int e = st->vision.error_x1000;
        ui_label_set_fmt(s_vis.err, "ERROR %c%d.%03d", e < 0 ? '-' : '+',
                         (e < 0 ? -e : e) / 1000, (e < 0 ? -e : e) % 1000);
        int a = st->vision.angle_x10;
        ui_label_set_fmt(s_vis.ang, "ANGLE %c%d.%u deg", a < 0 ? '-' : '+',
                         (a < 0 ? -a : a) / 10, (unsigned)((a < 0 ? -a : a) % 10));
        ui_label_set_fmt(s_vis.conf, "CONF %u %%", st->vision.confidence);
        if (st->vision.mode == VISION_MODE_IDX_OBJECT) {
            ui_label_set_fmt(s_vis.objs, "OBJ %u", st->vision.objects_count);
        } else {
            ui_label_set_text(s_vis.objs, "OBJ --");
        }
    }

    int shown_drive = (fresh || st->conn != SCR_CONN_CONNECTED)
                          ? st->vision.drive_mode : 0;
    /* interlock (design 7.3): no fresh vision -> ASSIST cannot be shown on */
    if (!fresh) {
        shown_drive = 0;
    }
    ui_label_set_fmt(s_vis.mode_row, "ASSIST %s",
                     shown_drive == 1 ? "ON" : shown_drive == 2 ? "AUTO" : "OFF");

    /* button states: mode follows the gateway ack, gates follow CTRL */
    bool en = st->ctrl_role && st->conn == SCR_CONN_CONNECTED;
    for (int i = 0; i < VISION_MODE_IDX_COUNT; i++) {
        set_checked(s_vis.mode_btn[i], i == st->vision.mode);
        set_enabled(s_vis.mode_btn[i], en);
    }
    for (int i = 0; i < 3; i++) {
        set_checked(s_vis.drive_btn[i], i == shown_drive);
        bool ben = en && (i != 2) && (i != 1 || fresh);
        set_enabled(s_vis.drive_btn[i], ben);
    }

    const char *why = "";
    if (!en) {
        why = (st->conn != SCR_CONN_CONNECTED) ? "LINK DOWN" : "NEED CONTROL";
    } else if (!fresh && st->vision.mode != VISION_MODE_IDX_OFF) {
        why = "VISION STALE";
    }
    ui_label_set_text(s_vis.reason, why);
    ui_label_set_color(s_vis.reason, lv_color_hex(UI_COL_WARN));

    apply_layout(ui_video_immersive());
    ui_video_view_pump(&s_vis.view, st);
#if CONFIG_SCR_CAM_OVERLAY
    overlay_update(st);
#endif
}
