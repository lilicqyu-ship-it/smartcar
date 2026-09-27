#include "rt/servo.h"

typedef struct
{
    float32 integ;                     /* Ki*sum(e), duty pct*10, pre-clamped */
} ServoSide;

static ServoSide g_servo[SERVO_SIDES];

void SERVO_init(void)
{
    uint8 i;

    for (i = 0u; i < SERVO_SIDES; i++)
    {
        g_servo[i].integ = 0.0f;
    }
}

void SERVO_reset(uint8 side)
{
    if (side < SERVO_SIDES)
    {
        g_servo[side].integ = 0.0f;
    }
}

float32 SERVO_getIntegral(uint8 side)
{
    return (side < SERVO_SIDES) ? g_servo[side].integ : 0.0f;
}

sint16 SERVO_update(uint8 side, sint16 targetPct10, sint16 measPct10, boolean measValid)
{
    ServoSide *s;
    float32    e;
    float32    u;

    if (side >= SERVO_SIDES)
    {
        return 0;
    }
    s = &g_servo[side];

    if (!measValid)
    {
        /* Open-loop fallback: no trustworthy measurement means no loop. The
         * integral is dropped, not frozen - a charge accumulated against a
         * dead encoder would kick the wheel the moment it came back. */
        s->integ = 0.0f;
        return targetPct10;
    }

    e = (float32)targetPct10 - (float32)measPct10;

    /* Integrate outside the deadband only: within it the error is median-
     * window noise, and integrating it would just wind the I term up and
     * make the wheel hunt around zero. */
    if ((e > (float32)SERVO_E_DEADBAND) || (e < -(float32)SERVO_E_DEADBAND))
    {
        s->integ += SERVO_KI * e;
        if (s->integ > (float32)SERVO_INTEGRAL_MAX)
        {
            s->integ = (float32)SERVO_INTEGRAL_MAX;
        }
        else if (s->integ < -(float32)SERVO_INTEGRAL_MAX)
        {
            s->integ = -(float32)SERVO_INTEGRAL_MAX;
        }
    }

    u = (SERVO_FF_GAIN * (float32)targetPct10) + (SERVO_KP * e) + s->integ;

    if (u > (float32)SERVO_OUT_MAX)
    {
        u = (float32)SERVO_OUT_MAX;
    }
    else if (u < -(float32)SERVO_OUT_MAX)
    {
        u = -(float32)SERVO_OUT_MAX;
    }

    /* Below the output deadband a command is stiction noise: coast (0) and
     * let the I term decide when it is worth pushing again. */
    if ((u < (float32)SERVO_OUT_DEADBAND) && (u > -(float32)SERVO_OUT_DEADBAND))
    {
        return 0;
    }

    return (sint16)u;
}
