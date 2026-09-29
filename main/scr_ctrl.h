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

#ifdef __cplusplus
}
#endif

#endif /* SCR_CTRL_H */
