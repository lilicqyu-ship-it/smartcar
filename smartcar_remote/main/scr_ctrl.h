/*
 * scr_ctrl.h - vehicle control task: joystick -> DRIVE 0x50 @30 Hz + safety.
 *
 * UI is a View, this is the Controller (spec 73/74): page switching never
 * stops the control loop.  STOP is latching, emergency needs an explicit
 * release, radio loss stops the car (the DRIVE heartbeat stops feeding the
 * TC275 watchdog when the link is down) and shows a full-screen overlay.
 */
#ifndef SCR_CTRL_H
#define SCR_CTRL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void scr_ctrl_start(void);

/* STOP button: one touch stops now (spec 19).  Latched until the user
 * touches the joystick again. */
void scr_ctrl_stop_button(void);

/* Long-press STOP: emergency stop.  Inputs freeze, overlay demands an
 * explicit release (spec 19/63). */
void scr_ctrl_emergency(void);

/* Explicit emergency release from the overlay.  The vehicle stays stopped
 * until the user re-engages the joystick (spec 105). */
void scr_ctrl_emergency_release(void);

/* User touched the joystick: clears the STOP latch (never the emergency). */
void scr_ctrl_joystick_touch(void);

/* Called when the phone takes over control: the S3 must not send anything
 * and must show NO CONTROL (spec 103/104).  Handled via app_state owner;
 * this hook only latches a stop so nothing keeps driving. */
void scr_ctrl_control_lost(void);

/* Calibration page motor jog (DPT 0x71, doc/08 §5).  motor 0..3 = front-left,
 * front-right, rear-left, rear-right; duty = percent x10 (+-500 max).  While
 * set, the 30 Hz ctrl task sends 0x71 every tick next to the 0/0 heartbeat;
 * motor < 0 stops (one duty-0 frame is sent).  The TC275 auto-stops after
 * 300 ms without a jog frame, so a stalled UI can never leave a motor on. */
void scr_ctrl_set_jog(int motor, int16_t duty);

/* Joystick value changed: wake the ctrl task to send DRIVE immediately
 * (coalesced to <= 50 Hz).  Safe from the LVGL task. */
void scr_ctrl_kick(void);

#ifdef __cplusplus
}
#endif

#endif /* SCR_CTRL_H */
