#ifndef MOTOR_ALGO_H
#define MOTOR_ALGO_H

#include "Ifx_Types.h"

/* Motor drive algorithm - runs on CPU1 in a bare-metal 1 kHz loop.
 *
 * Inputs  : side speed targets (-1000..+1000) from the CPU0 control task via
 *           xcore, plus the CPU2 e-stop bypass flag.
 * Outputs : TB6612 PWM/direction pins via the motor BSP, and the applied
 *           speeds published back to the shared status block.
 *
 * Safety behaviour (implemented here so actuation stays on one core):
 *  - slew-rate limited ramps towards the commanded side speeds
 *  - immediate brake when an e-stop is latched
 *  - if CPU0 stops publishing targets (control task dead / stalled) the
 *    algorithm ramps to stop after ALGO_CMD_TIMEOUT_MS */

#define MOTOR_ALGO_PERIOD_MS     1       /* algorithm loop period */
#define MOTOR_ALGO_MAX_STEP      2       /* max speed change per ms (0..1000 in 0.5 s) */
#define MOTOR_ALGO_CMD_TIMEOUT_MS 150    /* targets stale for this long -> ramp to stop */

void MOTOR_ALGO_init(void);
void MOTOR_ALGO_task(void);              /* one 1 kHz step */
void MOTOR_ALGO_run(void);               /* never returns */

#endif
