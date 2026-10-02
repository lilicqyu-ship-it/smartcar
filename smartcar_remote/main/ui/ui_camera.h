/*
 * ui_camera.h - CAMERA page (Remote design doc 8.2): 640x480 video area +
 * 160 px sidebar (profile / vision page link / link state).  No PHOTO/REC/SD:
 * the S3-CAM has no MicroSD, storage features dropped.
 *
 * Also exports the shared video-view widget (frame-slot pump + NO SIGNAL
 * mask) reused by ui_vision.c, and the immersive flag that hides the tab
 * bar while a 640x480 stream fills the left edge.
 */
#ifndef UI_CAMERA_H
#define UI_CAMERA_H

#include <stdbool.h>
#include "lvgl.h"
#include "../app_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared 1:1 video view: black area, centred RGB565 image, NO SIGNAL mask.
 * The dsc references scr_cam slot memory - owned and retired by scr_cam. */
typedef struct {
    lv_obj_t      *area;
    lv_obj_t      *img;
    lv_obj_t      *mask;        /* full-area overlay label, hidden while live */
    lv_image_dsc_t dsc;
    uint32_t       cur_seq;     /* frame the image currently references       */
    bool           has_frame;
} ui_video_view_t;

/* parent must be the sized black video area; the image is centred in it. */
void ui_video_view_create(ui_video_view_t *v, lv_obj_t *parent);
/* Acquire the newest decoded frame; a changed seq is the only repaint source
 * (seq unchanged -> no invalidate, the RGB bounce feed stays quiet). */
void ui_video_view_pump(ui_video_view_t *v, const scr_state_t *st);

void ui_camera_create(lv_obj_t *root);
void ui_camera_refresh(const scr_state_t *st);

/* true while a full-height (WEB_PREVIEW) stream is displayed: the tab bar
 * hides and the page layout goes immersive.  Driven by the cam dims. */
bool ui_video_immersive(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_CAMERA_H */
