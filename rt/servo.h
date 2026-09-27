#ifndef SERVO_H
#define SERVO_H

#include "Ifx_Types.h"

/* Closed-loop wheel speed servo, one instance per side (doc 21 SS5.2).
 *
 * Domain: percent*10 everywhere. The commanded side target from CPU0, the
 * duty handed to MOTOR_setSpeed and the measured speed from the encoder's
 * percent channel are all the same unit, which keeps every gain dimensionless
 * (1 pct*10 of duty per 1 pct*10 of speed). Physical mm/s enters only through
 * ENCODER_FULL_SCALE_MM_S (rt/encoder.h), the constant that defines the
 * percent domain - so retuning that constant retunes the whole loop.
 *
 * Structure per side (doc 21 SS5.2):
 *   slewed target -> [ feedforward FF_GAIN*target + Kp*e + Ki*integral ]
 *   e = target - measured; integral anti-windup clamp +-30% duty; output
 *   clamp +-1000 (same clamp as the motor BSP - the "three layers agree"
 *   rule) and a small output deadband so the loop does not chatter the
 *   TB6612 direction pins around zero.
 *
 * Fallback (the piece that makes this safe to ship while the 8 encoder
 * lines are still unwired): when the encoder is not alive the loop has no
 * trustworthy measurement, so SERVO_update degrades to the previous
 * open-loop behaviour - duty = target - and drops the integral instead of
 * freezing it, so no stale charge kicks the wheel on re-lock.
 *
 * Gains are first-guess values pending the bench tune (doc 21 SS15.3):
 * with a roughly linear plant (100% duty ~ full-scale speed) Kp = 0.8 and
 * Ki = 0.01/ms follow the 0.5 s slew ramp without hunting. Tune on the
 * bench from the SRV= log line, not here. */

#define SERVO_SIDES            2

#define SERVO_OUT_MAX          1000    /* duty pct*10, BSP clamp twin          */
#define SERVO_OUT_DEADBAND     5       /* <0.5% duty: coast instead of dither  */
#define SERVO_INTEGRAL_MAX     300     /* +-30% duty (doc 21 SS5.2)            */
#define SERVO_E_DEADBAND       3       /* <=0.3% full scale: noise, not error  */

#define SERVO_KP               0.8f    /* duty per speed error, both pct*10    */
#define SERVO_KI               0.01f   /* per ms: e=100 adds 1 duty per ms     */
#define SERVO_FF_GAIN          1.0f    /* duty = gain * target, plant guess    */

void    SERVO_init(void);
/* One 1 kHz step for one side. Returns the duty to apply (percent*10). */
sint16  SERVO_update(uint8 side, sint16 targetPct10, sint16 measPct10, boolean measValid);
void    SERVO_reset(uint8 side);       /* drop the integral (brake/e-stop)      */
float32 SERVO_getIntegral(uint8 side); /* bench observation of the I term       */

#endif /* SERVO_H */
