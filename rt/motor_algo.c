#include "rt/motor_algo.h"
#include "bsp/motor.h"
#include "rt/encoder.h"
#include "bsp/stime.h"
#include "bsp/wdg.h"
#include "mw/xcore/xcore.h"

/* Left side: MOTOR_A + MOTOR_B (TB6612#1), Right side: MOTOR_C + MOTOR_D (TB6612#2)
 * (same wheel mapping the robot controller uses) */
#define SIDE_LEFT_0   MOTOR_A
#define SIDE_LEFT_1   MOTOR_B
#define SIDE_RIGHT_0  MOTOR_C
#define SIDE_RIGHT_1  MOTOR_D

typedef struct
{
    sint16  current;                     /* applied side speed, slew limited */
    sint16  target;                      /* commanded side speed */
} AlgoSide;

static AlgoSide g_left;
static AlgoSide g_right;

static uint32  g_lastSeq;                /* last seen target update counter */
static uint32  g_lastSeqMs;              /* timestamp of the last target update */
static boolean g_targetEstop;            /* e-stop bit of the last published target */

static sint16 MOTOR_ALGO_stepToward(sint16 current, sint16 target)
{
    sint16 diff = target - current;

    if (diff > MOTOR_ALGO_MAX_STEP)
    {
        diff = MOTOR_ALGO_MAX_STEP;
    }
    else if (diff < -MOTOR_ALGO_MAX_STEP)
    {
        diff = -MOTOR_ALGO_MAX_STEP;
    }
    return current + diff;
}

static void MOTOR_ALGO_apply(sint16 left, sint16 right)
{
    MOTOR_setSpeed(SIDE_LEFT_0, left);
    MOTOR_setSpeed(SIDE_LEFT_1, left);
    MOTOR_setSpeed(SIDE_RIGHT_0, right);
    MOTOR_setSpeed(SIDE_RIGHT_1, right);
}

static void MOTOR_ALGO_readTargets(void)
{
    sint16  l, r;
    boolean estop;
    uint32  seq = XCORE_motorGetTarget(&l, &r, &estop);
    uint32  now = STIME_nowMs();

    if (seq != g_lastSeq)
    {
        g_lastSeq   = seq;
        g_lastSeqMs = now;
    }
    g_targetEstop = estop;

    /* Control task lost -> ramp to stop (safety) */
    if ((now - g_lastSeqMs) > MOTOR_ALGO_CMD_TIMEOUT_MS)
    {
        l = 0;
        r = 0;
    }

    g_left.target  = l;
    g_right.target = r;
}

static void MOTOR_ALGO_brakeAll(void)
{
    g_left.current  = 0;
    g_right.current = 0;
    MOTOR_brake(SIDE_LEFT_0);
    MOTOR_brake(SIDE_LEFT_1);
    MOTOR_brake(SIDE_RIGHT_0);
    MOTOR_brake(SIDE_RIGHT_1);
    XCORE_motorStatusSet(0, 0);
}

void MOTOR_ALGO_init(void)
{
    g_left.current   = 0;
    g_right.current  = 0;
    g_left.target    = 0;
    g_right.target   = 0;
    g_lastSeq        = 0;
    g_lastSeqMs      = STIME_nowMs();
    g_targetEstop    = FALSE;

    MOTOR_stopAll();
}

void MOTOR_ALGO_task(void)
{
    /* Encoder tick first: it must run at 1 kHz regardless of the e-stop
     * branch below, and its measured speeds feed the telemetry. */
    ENCODER_task();

    MOTOR_ALGO_readTargets();

    /* E-stop (CPU2 bypass bit or the bit published by CPU0): brake at once,
     * no ramp. */
    if (g_targetEstop || XCORE_estopIsActive())
    {
        MOTOR_ALGO_brakeAll();
        return;
    }

    g_left.current  = MOTOR_ALGO_stepToward(g_left.current, g_left.target);
    g_right.current = MOTOR_ALGO_stepToward(g_right.current, g_right.target);

    MOTOR_ALGO_apply(g_left.current, g_right.current);
    XCORE_motorStatusSet(g_left.current, g_right.current);
}

void MOTOR_ALGO_run(void)
{
    uint32 nextMs = STIME_nowMs();

    while (1)
    {
        MOTOR_ALGO_task();
        WDG_serviceCpu();               /* CPU1 WDT feed point, 1 kHz (SDD SS7.2) */

        nextMs += MOTOR_ALGO_PERIOD_MS;
        STIME_waitUntilMs(nextMs);
    }
}
