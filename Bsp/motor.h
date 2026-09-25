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

#endif