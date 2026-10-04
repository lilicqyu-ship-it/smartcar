#ifndef MOTOR_H
#define MOTOR_H

#include "Ifx_Types.h"

typedef enum
{
    MOTOR_A = 0,            /* Front 1:  PWMA P21.0, AIN1 P21.4, AIN2 P21.5 */
    MOTOR_B,                /* Rear  1:  PWMB P21.3, BIN1 P21.2, BIN2 P22.3 */
    MOTOR_C,                /* Rear  2:  PWMA P00.0, AIN1 P00.2, AIN2 P00.6 */
    MOTOR_D,                /* Front 2:  PWMB P00.8, BIN1 P00.10, BIN2 P00.12 */
    MOTOR_COUNT
} MotorId;

void MOTOR_init(void);
void MOTOR_setSpeed(MotorId id, sint16 speed);  /* speed in range -1000..+1000, sign = direction */
void MOTOR_brake(MotorId id);
void MOTOR_stop(MotorId id);
void MOTOR_stopAll(void);

/* TB6612 STBY (P22.2 -> D24A J4-2), one line for all four channels.
 * FALSE = standby: outputs high-impedance and IN/PWM ignored (MOTOR_setEnabled
 * stops every channel first). TRUE = armed: MOTOR_brake/stop take effect.
 * MOTOR_init arms the driver as its last step, so the channels stay in
 * standby until CPU1 has run its BSP init. */
void MOTOR_setEnabled(boolean enabled);

#endif