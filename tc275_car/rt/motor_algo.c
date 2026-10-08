#include "rt/motor_algo.h"
#include "bsp/motor.h"
#include "rt/encoder.h"
#include "rt/servo.h"
#include "bsp/stime.h"
#include "bsp/adc.h"
#include "bsp/imu.h"
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

/* ---- per-motor open-loop jog, 0x71 (doc 34 SS9.3) --------------------------
 * Bench tool: CPU0 writes clamped +-500 duties, this core drives them
 * directly (open loop, no servo, encoders not involved) while the command
 * stream is fresh. Priority: estop > calibration > jog > servo. 300 ms
 * without a refresh zeroes the duties and the servo path resumes. */
#define MOTOR_JOG_TIMEOUT_MS    300u

static uint32  g_jogSeq;                 /* last seen jog command counter    */
static uint32  g_jogDeadlineMs;          /* 0 = jog inactive                 */
static uint8   g_recordVer = 0xFFu;      /* impossible value: apply boot rec */

/* Closed-loop enable gate. The servo may only close the loop after wheel
 * direction calibration. An IMU-only DFlash record must not open it: the
 * default wheel signs can be wrong on mirrored gearboxes, and closing the loop on
 * wrong signs is exactly the positive-feedback runaway bench-observed in
 * doc 34 SS1 (sustained duty at target 0). Doc 23 SS8.4 therefore
 * prescribes "open-loop equivalent" until wheelCalibrated=1: SERVO_update's measValid=
 * FALSE path (duty = target, integral dropped) is reused unchanged. The
 * gate follows the RecordLive version edge - 0x70 DONE and 0x73 REC_SET
 * enable it, 0x74 REC_CLEAR disables it; IMU 0x7A leaves it untouched. */
static boolean g_closedLoopOk;

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

static boolean g_servoLogAsked;          /* 5-second bench line, only when active */

/* Bench stream period: one compact SRVB line every 10 ms (100 Hz) while the
 * ASCLIN0 console has BENCH on (app/console.c, MATLAB live_serial_plot.m).
 * ~60 bytes/line -> ~6 KB/s against the ~11.5 KB/s the 115200 console can
 * carry, leaving headroom for the regular log traffic; the uptime column
 * lets the receiver see (and reject) whole lines the ring had to drop. */
#define MOTOR_ALGO_BENCH_PERIOD_MS    10u

/* One named SERVO line every 5 seconds while anything is moving: target, measured and
 * applied duty per side (percent*10), the figures the bench tune of doc 21
 * SS15.3 needs. Idle robot stays silent so the console does not spam.
 * BENCH on replaces this line with the 10 ms SRVB stream, which ignores the
 * idle gate (a continuous timebase is the point of a plot) and folds in the
 * per-side integral (x10, 0 during jog where the servo is not stepping). */
static void MOTOR_ALGO_diag(XcoreEncoder *enc, sint16 dutyL, sint16 dutyR,
                            float32 integL, float32 integR)
{
    static uint32 nextLogMs;
    static uint32 nextBenchMs;
    uint32        now = STIME_nowMs();

    if (XCORE_benchLogActive())
    {
        if ((sint32)(now - nextBenchMs) >= 0)
        {
            sint32 vals[9];

            vals[0] = (sint32)now;
            vals[1] = g_left.target;
            vals[2] = enc->pctLeft;
            vals[3] = dutyL;
            vals[4] = (sint32)(integL * 10.0f);
            vals[5] = g_right.target;
            vals[6] = enc->pctRight;
            vals[7] = dutyR;
            vals[8] = (sint32)(integR * 10.0f);
            XCORE_logi("SRVB", vals, 9u);

            nextBenchMs = now + MOTOR_ALGO_BENCH_PERIOD_MS;
        }
        return;
    }

    if ((g_left.target == 0) && (g_right.target == 0) &&
        (dutyL == 0) && (dutyR == 0) &&
        (enc->pctLeft == 0) && (enc->pctRight == 0))
    {
        /* Keep the rate gate across short idle gaps / encoder jitter. */
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
        XCORE_LOG_FIELDS("[SERVO]", XL_U("uptime_ms", now),
            XL_I("target_left_pct_x10", vals[0]), XL_I("measured_left_pct_x10", vals[1]),
            XL_I("duty_left_pct_x10", vals[2]), XL_I("target_right_pct_x10", vals[3]),
            XL_I("measured_right_pct_x10", vals[4]), XL_I("duty_right_pct_x10", vals[5]));

        nextLogMs       = now + 5000u;
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
     * from this very cycle. measValid=FALSE (the side's sensors silent while
     * it is commanded, OR the direction record still src=0) selects the
     * open-loop fallback inside SERVO_update - duty = target, integral
     * dropped - not an error. The check is per side (doc 51): the merged
     * alive flag let a moving opposite side mask a dead one, and the servo
     * then integrated against that side's fake 0 mm/s. A side with no
     * command has nothing to measure, so cmd==0 always passes. */
    XCORE_encoderRead(&enc);

    {
        boolean measOkL = g_closedLoopOk &&
            ((enc.edgeAgeMs[0] <= ENCODER_EDGE_FRESH_MS) || (g_left.cmd == 0));
        boolean measOkR = g_closedLoopOk &&
            ((enc.edgeAgeMs[1] <= ENCODER_EDGE_FRESH_MS) || (g_right.cmd == 0));

        dutyL = SERVO_update(0u, g_left.target, enc.pctLeft, measOkL);
        dutyR = SERVO_update(1u, g_right.target, enc.pctRight, measOkR);
    }

    MOTOR_ALGO_apply(dutyL, dutyR);
    XCORE_motorStatusSet(dutyL, dutyR);
    MOTOR_ALGO_diag(&enc, dutyL, dutyR, SERVO_getIntegral(0u), SERVO_getIntegral(1u));
}

/* ---- direction calibration ------------------------------------------------- */

/* One result block per run / reject (doc 34 SS3.3): CPU0 consumes it, turns
 * it into EVT 0x22 and auto-persists on success. Invert values are read
 * after the pulse test, so a DONE block carries the sign terminal. */
static void MOTOR_ALGO_calibPublish(uint8 status)
{
    XcoreCalibResult res;
    uint8          i;

    res.status = status;
    for (i = 0u; i < MOTOR_COUNT; i++)
    {
        res.invert[i] = ENCODER_getInvert(i);
        res.delta[i]  = g_calib.delta[i];   /* untested wheels read 0       */
    }
    XCORE_calibResultPublish(&res);
}

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
    XCORE_benchSetActive(TRUE);
    g_calib.wheel        = 0u;
    g_calib.phase        = CALIB_PULSE;
    g_calib.phaseStartMs = STIME_nowMs();
    g_calib.countAtStart = counts[0];

    /* Drop a request latched while the previous run was still active: the
     * busy answer goes out from MOTOR_ALGO_task, the run is not restarted. */
    (void)XCORE_dirCalibConsume();

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

    /* The delta is POST-invert, so a negative one means the CURRENT sign is
     * wrong - flip it, do not force -1: a wheel that boots with a stale -1
     * from the DFlash record would otherwise stay -1 forever (re-running the
     * calibration could never repair it, and that wheel's mm/s keeps
     * cancelling its side partner -> body speed reads ~0 or negative). */
    if (g_calib.delta[g_calib.wheel] < 0)
    {
        ENCODER_setInvert(g_calib.wheel, (sint8)(-ENCODER_getInvert(g_calib.wheel)));
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
                XCORE_LOG_FIELDS("[ENCODER_CALIBRATION]",
                    XL_I("invert_A", vals[0]), XL_I("invert_B", vals[1]),
                    XL_I("invert_C", vals[2]), XL_I("invert_D", vals[3]),
                    XL_I("delta_A_counts", vals[4]), XL_I("delta_B_counts", vals[5]),
                    XL_I("delta_C_counts", vals[6]), XL_I("delta_D_counts", vals[7]));   /* invert[0..3] delta[0..3] */

                MOTOR_ALGO_calibPublish(CALIB_STATUS_DONE);
                g_calib.active = FALSE;
                XCORE_benchSetActive(FALSE);
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
        XCORE_benchSetActive(FALSE);
        break;
    }
}

static void MOTOR_ALGO_calibAbort(void)
{
    if (g_calib.active)
    {
        g_calib.active = FALSE;
        XCORE_benchSetActive(FALSE);
        g_calib.phase  = CALIB_IDLE;
        MOTOR_stopAll();
        MOTOR_ALGO_calibPublish(CALIB_STATUS_ABORTED);
        XCORE_logln("ENCCAL aborted (estop)");
    }
}

/* ---- calibration record apply (doc 34 SS8.3) -------------------------------
 * CPU0 loads the DFlash record at boot and republishes it on every change;
 * this core applies it on the version edge: signs go to the encoder, the
 * two speed-conversion parameters to their runtime setters, and the closed-
 * loop gate follows src (see g_closedLoopOk above). Anything outside the
 * legal range is clamped there, so a corrupt record cannot reach the
 * 1 kHz math. Position bytes stay metadata (doc 34 SS8.4). */
static void MOTOR_ALGO_applyRecord(void)
{
    XcoreRecordLive live;
    uint8           i;

    XCORE_recordGet(&live);
    if (live.version == g_recordVer)
    {
        return;
    }
    g_recordVer = live.version;

    {
        boolean ok = (live.rec.wheelCalibrated != 0u) ? TRUE : FALSE;

        if (ok != g_closedLoopOk)
        {
            g_closedLoopOk = ok;
            XCORE_logln(ok ? "SERVO closed-loop enabled"
                           : "SERVO open-loop (wheels uncalibrated; calibrate via 0x70)");
        }
    }

    for (i = 0u; i < MOTOR_COUNT; i++)
    {
        ENCODER_setInvert(i, live.rec.invert[i]);
    }
    ENCODER_setFullScaleMmS((sint32)live.rec.fullScaleMmS);
    ENCODER_setWheelDiaMm((sint32)live.rec.wheelDiaMm);
}

/* ---- per-motor jog ----------------------------------------------------------- */

/* Hand the outputs back: the local deadline goes inactive and the shared
 * duties are zeroed, so CPU0's echo and the quiet gate see a stopped jog.
 * Only called when a jog stream is actually armed (the 1 kHz loop must not
 * take the xcore lock every tick while e-stopped). */
static void MOTOR_ALGO_jogStop(void)
{
    g_jogDeadlineMs = 0u;
    XCORE_jogClear();
}

/* TRUE while 0x71 is in charge of the outputs. Duty sign is the motor's own
 * forward direction, already clamped to +-500 by CPU0; open loop on purpose
 * (bench wiring check), so the servo and the encoder play no part here. */
static boolean MOTOR_ALGO_jogStep(void)
{
    XcoreJog jog;
    uint32   now = STIME_nowMs();

    if (XCORE_jogGet(&jog) != g_jogSeq)
    {
        g_jogSeq        = jog.jogSeq;
        g_jogDeadlineMs = now + MOTOR_JOG_TIMEOUT_MS;
    }
    if (g_jogDeadlineMs == 0u)
    {
        return FALSE;                    /* never started / already expired  */
    }
    if ((sint32)(now - g_jogDeadlineMs) >= 0)
    {
        MOTOR_ALGO_jogStop();
        SERVO_reset(0u);
        SERVO_reset(1u);
        MOTOR_stopAll();
        XCORE_logln("JOG timeout");
        return FALSE;
    }

    MOTOR_setSpeed(MOTOR_A, jog.duty[0]);
    MOTOR_setSpeed(MOTOR_B, jog.duty[1]);
    MOTOR_setSpeed(MOTOR_C, jog.duty[2]);
    MOTOR_setSpeed(MOTOR_D, jog.duty[3]);
    XCORE_motorStatusSet((sint16)(jog.duty[0] + jog.duty[1]),
                         (sint16)(jog.duty[2] + jog.duty[3]));

    /* SRV= stays available for the bench (doc 34 SS9.3): targets read 0, the
     * duty column shows the jog values actually applied. The servo is not
     * stepping during a jog, so the integral columns report 0. */
    {
        XcoreEncoder enc;

        XCORE_encoderRead(&enc);
        MOTOR_ALGO_diag(&enc, jog.duty[0] + jog.duty[1], jog.duty[2] + jog.duty[3],
                        0.0f, 0.0f);
    }
    return TRUE;
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
        XCORE_benchSetActive(FALSE);
    g_calib.wheel        = 0u;
    g_calib.phase        = CALIB_IDLE;
    g_calib.phaseStartMs = 0u;
    g_calib.countAtStart = 0;

    g_jogSeq        = 0u;
    g_jogDeadlineMs = 0u;
    g_recordVer     = 0xFFu;             /* force the first record apply     */
    g_closedLoopOk  = FALSE;             /* gate opens on the first record   */

    SERVO_init();
    ADC_init();                          /* battery telemetry, CPU1-owned     */
    IMU_init();                          /* six-axis IMU on QSPI1, CPU1-owned */
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

    /* IMU sample: a 14-byte QSPI1 burst every 5th tick (~120 us), converted
     * and published to the xcore IMU block (doc 35). Like ADC_task it must
     * run in every branch, so it sits before the e-stop return. */
    IMU_task();

    /* Parameter/invert record from CPU0's DFlash load (cheap version peek). */
    MOTOR_ALGO_applyRecord();

    MOTOR_ALGO_readTargets();

    /* E-stop (CPU2 bypass bit or the bit published by CPU0): brake at once,
     * no ramp - and kill any calibration run or jog stream in progress. */
    if (g_targetEstop || XCORE_estopIsActive())
    {
        MOTOR_ALGO_calibAbort();
        if (g_jogDeadlineMs != 0u)
        {
            MOTOR_ALGO_jogStop();
        }
        MOTOR_ALGO_brakeAll();
        SERVO_reset(0u);
        SERVO_reset(1u);
        return;
    }

    if (g_calib.active)
    {
        /* A 0x70 latched during a run is refused with a busy result, never a
         * restart (doc 34 SS3.3). */
        if (XCORE_dirCalibConsume())
        {
            MOTOR_ALGO_calibPublish(CALIB_STATUS_BUSY);
        }
        MOTOR_ALGO_calibStep();          /* calib owns the outputs (~1.4 s)  */
        return;
    }
    if (XCORE_dirCalibConsume())
    {
        MOTOR_ALGO_calibStart();
        return;
    }
    if (MOTOR_ALGO_jogStep())
    {
        return;                          /* jog owns the outputs (0x71)      */
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
