/*
 * scr_cam.h - Camera WS client: /ws/camera preview plane -> JPEG decode -> slot ring
 *
 * Second, fully independent WebSocket connection next to scr_link's control WS
 * (S3CAM LLDD 8.2, Remote design doc 4/5).  The video plane can never stall
 * the control plane: separate client instance, separate tasks, separate
 * watchdogs, and NO camera condition ever reaches the safety state machine
 * (design R-ADR-04).
 *
 * Data path:
 *   cam_ws client task (core 0)   reassembly + header parse + seq accounting
 *        | pend slot (depth 1, latest-only)
 *   cam_decode task (core 0)      esp_new_jpeg PIE SIMD (TJpgDec fallback) -> RGB565
 *        | newest published
 *   LVGL timer (core 1)           scr_cam_display_acquire() swaps in newest
 *
 * Ownership rule: scr_cam owns every camera framebuffer.  The LVGL task only
 * borrows the currently-displayed slot; the decoder never writes into it.
 */
#ifndef SCR_CAM_H
#define SCR_CAM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the video-plane client (non-blocking).  Called from app_main after
 * scr_link_start().  No-op when CONFIG_SCR_CAM_WS_ENABLE=n. */
void scr_cam_start(void);

/* Preview subscription tied to page visibility (LVGL context).
 * enter: want the stream -> subscribe (or on the next CONNECTED edge).
 * leave: pause; the WS itself stays up (design R-ADR-06). */
void scr_cam_page_enter(void);
void scr_cam_page_leave(void);

/* Camera control.  Callers must check st.ctrl_role in the UI; these just send.
 * Profile rides the CAMERA WS text plane (LLDD 8.2).  Snapshot/record are
 * dropped with the SD feature: the S3-CAM has no MicroSD to store to. */
bool scr_cam_cmd_profile(const char *name);   /* CAM_PROFILE_REMOTE / WEB */

/* Borrow the newest decoded frame for display.  LVGL context only.
 * Returns a RGB565 buffer (w*h*2 bytes) and sets w, h and seq, or NULL when
 * no new frame is available.  When a newer seq is published, the previously
 * returned slot is retired on the next call: never touch the old pointer
 * after the returned seq changed. */
const uint8_t *scr_cam_display_acquire(uint16_t *w, uint16_t *h, uint32_t *seq);

#ifdef __cplusplus
}
#endif

#endif /* SCR_CAM_H */
