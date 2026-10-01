#ifndef MOTOR_ALGO_H
#define MOTOR_ALGO_H

#include "Ifx_Types.h"

/* Motor drive task - runs on CPU1 in a bare-metal 1 kHz loop.
 *
 * Inputs  : side speed targets (-1000..+1000, percent*10) from the CPU0
 *           control task via xcore, plus the CPU2 e-stop bypass flag.
 * Outputs : TB6612 PWM/direction pins via the motor BSP, and the applied
 *           duties published back to the shared status block.
 *
 * Control (doc 21 SS5.2): the slew-limited target feeds rt/servo, which
 * closes the loop against the encoder's measured side speed and returns the
 * duty. With the encoder not alive the servo degrades to duty = target, the
 * previous open-loop behaviour, so the drivetrain works before the 8
 * encoder lines are wired and while an encoder is dead.
 *
 * Safety behaviour (implemented here so actuation stays on one core):
 *  - slew-rate limited ramps towards the commanded side speeds
 *  - immediate brake when an e-stop is latched
 *  - if CPU0 stops publishing targets (control task dead / stalled) the
 *    algorithm ramps to stop after MOTOR_ALGO_CMD_TIMEOUT_MS
 *
 * Bench support: PROTO 0x70 (via XCORE_dirCalibConsume) starts an automated
 * wheel-direction calibration - a +12% duty pulse per wheel, counts sign
 * checked, g_encInvert flipped, result on the console log ("ENCCAL="). This
 * is doc 23 SS8.4 step 2 automated (doc 21 SS15.3 产测自动判向) and REQUIRES
 * THE WHEELS OFF THE GROUND: for ~1.4 s every wheel is commanded in turn
 * regardless of the CPU0 targets.
 *
 * Doc 34 additions, all bench-only:
 *  - the calibration outcome is also published cross-core
 *    (XCORE_calibResultPublish) so CPU0 can answer EVT 0x22 and persist it;
 *    a request while a run is in progress is refused (CALIB_STATUS_BUSY),
 *    never restarted.
 *  - PROTO 0x71 (via XCORE_jogGet) drives one motor open loop, bypassing the
 *    servo, until the command stream goes stale for MOTOR_JOG_TIMEOUT_MS.
 *  - the live calibration record (XCORE_recordGet) feeds rt/encoder's
 *    full-scale and wheel-diameter variables on a version edge.
 * Output ownership priority: e-stop > calibration > jog > servo. */

#define MOTOR_ALGO_PERIOD_MS     1       /* algorithm loop period */
#define MOTOR_ALGO_MAX_STEP      2       /* max speed change per ms (0..1000 in 0.5 s) */
#define MOTOR_ALGO_CMD_TIMEOUT_MS 150    /* targets stale for this long -> ramp to stop */

void MOTOR_ALGO_init(void);
void MOTOR_ALGO_task(void);              /* one 1 kHz step */
void MOTOR_ALGO_run(void);               /* never returns */

#endif
