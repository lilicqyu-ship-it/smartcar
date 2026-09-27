#ifndef ENCODER_H
#define ENCODER_H

#include "Ifx_Types.h"

/* MG310 wheel Hall encoders (doc/20-design/23-wiring.md section 8):
 * 260 lines * gear 1:20.409 per wheel rev, A/B quadrature, x4 decode here
 * (both edges of both phases), pins P33.0..P33.7 = X2-28..35. */
#define ENCODER_COUNT              4      /* E1..E4, one per motor */
#define ENCODER_LINES              260u
#define ENCODER_GEAR_NUM           20409u /* gear 1:20.409 */
#define ENCODER_GEAR_DEN           1000u
#define ENCODER_COUNTS_WHEEL_REV   ((uint32)((uint32)(ENCODER_LINES * 4u) * ENCODER_GEAR_NUM / ENCODER_GEAR_DEN))

/* Measured wheel speed that maps to percent*10 = 1000 in the telemetry domain.
 * Default assumes ~1000 mm/s at full command; recalibrate on the bench
 * (doc/20-design/21-software-design.md section 15.3 speed calibration). */
#define ENCODER_FULL_SCALE_MM_S    1000

/* A side is E1+E2 (motors A+B), right side is E3+E4 (motors C+D) - same split
 * as the motor algorithm. */
#define ENCODER_SIDE_LEFT_0        0u
#define ENCODER_SIDE_LEFT_1        1u
#define ENCODER_SIDE_RIGHT_0       2u
#define ENCODER_SIDE_RIGHT_1       3u

void    ENCODER_init(void);                   /* CPU1, after MOTOR_init (GTM CLK0) */
void    ENCODER_task(void);                   /* CPU1 1 kHz step: window, publish  */
void    ENCODER_publish(void);                /* push snapshot through xcore       */
void    ENCODER_getSpeedsMmS(sint32 v[2]);     /* left/right side, mm/s            */
void    ENCODER_getRawCounts(sint32 c[4]);     /* E1..E4 signed x4 counts           */
void    ENCODER_getOdometer(uint32 m[2]);     /* left/right side, mm               */
boolean ENCODER_isAlive(void);                /* edges seen within the alive window */

/* Direction calibration of one wheel's count sign (doc 23 section 8.4):
 * +1 = counts increase when the wheel turns "chassis forward". Runtime-
 * writable so the bench pulse test (0x70 -> motor_algo) flips a wheel
 * without a rebuild; CPU1-only writer, applied by the ISRs immediately. */
void    ENCODER_setInvert(uint8 enc, sint8 sign);
sint8   ENCODER_getInvert(uint8 enc);

#endif /* ENCODER_H */
