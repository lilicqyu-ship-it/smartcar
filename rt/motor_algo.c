#include "rt/motor_algo.h"
#include "bsp/motor.h"
#include "rt/encoder.h"
#include "rt/servo.h"
#include "bsp/stime.h"
#include "bsp/adc.h"
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
    sint16  cmd;                         /* raw side speed command from CPU0 */
    sint16  target;                      /* cmd slew-limited, feeds the servo */
} AlgoSide;

static AlgoSide g_left;
static AlgoSide g_right;

static uint32  g_lastSeq;                /* last seen target update counter */
static uint32  g_lastSeqMs;              /* timestamp of the last target update */
static boolean g_targetEstop;            /* e-stop bit of the last published target */

/* ---- wheel direction calibration (bench, doc 23 SS8.4 step 2 automated) ---
 * One +12% duty pulse per wheel for 250 ms; the count delta over the pulse
 * (already post-invert, the ISR applies g_encInvert) says which way that
 * wheel's encoder counts. A negative delta flips ENCODER_setInvert to -1, a
 * zero delta reports a dead encoder (no counts is wiring, not sign). Wheel
 * MOTOR_A..D is paired 1:1 with encoder E1..E4 (doc 23 SS8.2 table), so the
 * motor id doubles as the encoder index. Total run ~1.4 s; e-stop aborts it
 * from the normal branch above. */
#define MOTOR_CALIB_DUTY        120      /* 12% duty pulse, wheels OFF GROUND */
#define MOTOR_CALIB_PULSE_MS    250u
#define MOTOR_CALIB_SETTLE_MS   80u

typedef enum
{
    CALIB_IDLE = 0,
    CALIB_PULSE,                         /* driving the wheel under test     */
    CALIB_SETTLE                         /* stopped, let the wheel coast out */
} CalibPhase;

static struct
{
    boolean  active;
    uint8    wheel;                      /* MOTOR_A..MOTOR_D under test      */
    CalibPhase phase;
    uint32   phaseStartMs;
    sint32   countAtStart;
    sint32   delta[MOTOR_COUNT];         /* counts seen during the pulse     */
} g_calib;

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

    g_left.cmd  = l;
    g_right.cmd = r;
}

static void MOTOR_ALGO_brakeAll(void)
{
    g_left.cmd     = 0;
    g_right.cmd    = 0;
    g_left.target  = 0;
    g_right.target = 0;
    MOTOR_brake(SIDE_LEFT_0);
    MOTOR_brake(SIDE_LEFT_1);
    MOTOR_brake(SIDE_RIGHT_0);
    MOTOR_brake(SIDE_RIGHT_1);
    XCORE_motorStatusSet(0, 0);
}

/* ---- closed loop ----------------------------------------------------------- */

static boolean g_servoLogAsked;          /* 1 Hz bench line, only when active */

/* One SRV= line per second while anything is moving: target, measured and
 * applied duty per side (percent*10), the figures the bench tune of doc 21
 * SS15.3 needs. Idle robot stays silent so the console does not spam. */
static void MOTOR_ALGO_diag(XcoreEncoder *enc, sint16 dutyL, sint16 dutyR)
{
    static uint32 nextLogMs;
    uint32        now = STIME_nowMs();

    if ((g_left.target == 0) && (g_right.target == 0) &&
        (dutyL == 0) && (dutyR == 0) &&
        (enc->pctLeft == 0) && (enc->pctRight == 0))
    {
        g_servoLogAsked = FALSE;
        return;
    }

    if ((g_servoLogAsked == FALSE) || ((sint32)(now - nextLogMs) >= 0))
    {
        sint32 vals[6];

        vals[0] = g_left.target;
        vals[1] = enc->pctLeft;
        vals[2] = dutyL;
        vals[3] = g_right.target;
        vals[4] = enc->pctRight;
        vals[5] = dutyR;
        XCORE_logi("SRV=", vals, 6u);

        nextLogMs       = now + 1000u;
        g_servoLogAsked = TRUE;
    }
}

static void MOTOR_ALGO_controlStep(void)
{
    XcoreEncoder enc;
    sint16       dutyL;
    sint16       dutyR;

    /* Slew the target first: the servo sees a ramp, not a step, exactly like
     * the open-loop path did (0..1000 in 0.5 s at MAX_STEP=2/ms). */
    g_left.target  = MOTOR_ALGO_stepToward(g_left.target, g_left.cmd);
    g_right.target = MOTOR_ALGO_stepToward(g_right.target, g_right.cmd);

    /* ENCODER_task() ran at the top of MOTOR_ALGO_task, so this snapshot is
     * from this very cycle. alive=FALSE is the open-loop fallback inside
     * SERVO_update, not an error. */
    XCORE_encoderRead(&enc);

    dutyL = SERVO_update(0u, g_left.target, enc.pctLeft, enc.alive);
    dutyR = SERVO_update(1u, g_right.target, enc.pctRight, enc.alive);

    MOTOR_ALGO_apply(dutyL, dutyR);
    XCORE_motorStatusSet(dutyL, dutyR);
    MOTOR_ALGO_diag(&enc, dutyL, dutyR);
}

/* ---- direction calibration ------------------------------------------------- */

static void MOTOR_ALGO_calibStart(void)
{
    sint32 counts[MOTOR_COUNT];
    uint8  i;

    ENCODER_getRawCounts(counts);
    for (i = 0u; i < MOTOR_COUNT; i++)
    {
        g_calib.delta[i] = 0;
    }

    g_calib.active       = TRUE;
    g_calib.wheel        = 0u;
    g_calib.phase        = CALIB_PULSE;
    g_calib.phaseStartMs = STIME_nowMs();
    g_calib.countAtStart = counts[0];

    SERVO_reset(0u);
    SERVO_reset(1u);
    MOTOR_stopAll();
    XCORE_logln("ENCCAL start (wheels must be off ground)");
}

/* One wheel pulse finished: read the count delta, fix the sign, move on. */
static void MOTOR_ALGO_calibEndPulse(void)
{
    sint32 counts[MOTOR_COUNT];

    MOTOR_stop((MotorId)g_calib.wheel);
    ENCODER_getRawCounts(counts);
    g_calib.delta[g_calib.wheel] = counts[g_calib.wheel] - g_calib.countAtStart;

    if (g_calib.delta[g_calib.wheel] < 0)
    {
        ENCODER_setInvert(g_calib.wheel, -1);
    }
    /* delta == 0 stays +1: the ENCCAL line reports it as a dead channel. */

    g_calib.phase        = CALIB_SETTLE;
    g_calib.phaseStartMs = STIME_nowMs();
}

static void MOTOR_ALGO_calibStep(void)
{
    uint32 now = STIME_nowMs();

    switch (g_calib.phase)
    {
    case CALIB_PULSE:
        MOTOR_setSpeed((MotorId)g_calib.wheel, MOTOR_CALIB_DUTY);
        if ((now - g_calib.phaseStartMs) >= MOTOR_CALIB_PULSE_MS)
        {
            MOTOR_ALGO_calibEndPulse();
        }
        break;

    case CALIB_SETTLE:
        MOTOR_stop((MotorId)g_calib.wheel);
        if ((now - g_calib.phaseStartMs) >= MOTOR_CALIB_SETTLE_MS)
        {
            g_calib.wheel++;
            if (g_calib.wheel >= MOTOR_COUNT)
            {
                sint32 vals[8];
                uint8  i;

                for (i = 0u; i < MOTOR_COUNT; i++)
                {
                    vals[i] = ENCODER_getInvert(i);
                    vals[i + 4u] = g_calib.delta[i];
                }
                XCORE_logi("ENCCAL", vals, 8u);   /* invert[0..3] delta[0..3] */

                g_calib.active = FALSE;
                g_calib.phase  = CALIB_IDLE;
                MOTOR_stopAll();
            }
            else
            {
                sint32 counts[MOTOR_COUNT];

                ENCODER_getRawCounts(counts);
                g_calib.countAtStart = counts[g_calib.wheel];
                g_calib.phase        = CALIB_PULSE;
                g_calib.phaseStartMs = now;
            }
        }
        break;

    case CALIB_IDLE:
    default:
        g_calib.active = FALSE;
        break;
    }
}

static void MOTOR_ALGO_calibAbort(void)
{
    if (g_calib.active)
    {
        g_calib.active = FALSE;
        g_calib.phase  = CALIB_IDLE;
        MOTOR_stopAll();
        XCORE_logln("ENCCAL aborted (estop)");
    }
}

/* ---- public ---------------------------------------------------------------- */

void MOTOR_ALGO_init(void)
{
    g_left.cmd       = 0;
    g_right.cmd      = 0;
    g_left.target    = 0;
    g_right.target   = 0;
    g_lastSeq        = 0;
    g_lastSeqMs      = STIME_nowMs();
    g_targetEstop    = FALSE;
    g_servoLogAsked  = FALSE;

    g_calib.active       = FALSE;
    g_calib.wheel        = 0u;
    g_calib.phase        = CALIB_IDLE;
    g_calib.phaseStartMs = 0u;
    g_calib.countAtStart = 0;

    SERVO_init();
    ADC_init();                          /* battery telemetry, CPU1-owned     */
    MOTOR_stopAll();
}

void MOTOR_ALGO_task(void)
{
    /* Encoder tick first: it must run at 1 kHz regardless of the branch
     * taken below, and its measured speeds feed the servo and the telemetry. */
    ENCODER_task();

    /* Battery telemetry: a polled conversion every 10th tick (~3 us each),
     * filtered and published to the xcore battery block from here. It must
     * run in every branch, so it sits before the e-stop return. */
    ADC_task();

    MOTOR_ALGO_readTargets();

    /* E-stop (CPU2 bypass bit or the bit published by CPU0): brake at once,
     * no ramp - and kill any calibration run in progress. */
    if (g_targetEstop || XCORE_estopIsActive())
    {
        MOTOR_ALGO_calibAbort();
        MOTOR_ALGO_brakeAll();
        SERVO_reset(0u);
        SERVO_reset(1u);
        return;
    }

    if (g_calib.active)
    {
        MOTOR_ALGO_calibStep();          /* calib owns the outputs (~1.4 s)  */
        return;
    }
    if (XCORE_dirCalibConsume())
    {
        MOTOR_ALGO_calibStart();
        return;
    }

    MOTOR_ALGO_controlStep();
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
