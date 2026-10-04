#include "mw/xcore/xcore.h"
#include "bsp/uart.h"
#include "IfxCpu.h"

#include <string.h>

/* 16 slots: twice the drain rate of one CPU0 control period, so a burst of
 * superseded commands (the C6 packs a whole 512 B backlog segment after any
 * pump stall) saturates the coalescing path below before it saturates this
 * queue. Depth only buys time; correctness comes from XCORE_cmdPushLatest. */
#define XCORE_CMD_QUEUE_LEN   16
#define XCORE_LOG_RING_SIZE   2048U    /* power of two; holds several long lines */
/* One LINKDBG line is "LINKDBG=" + up to XCORE_LOG_MAX_VALS decimal u32 groups
 * (11 chars each) = ~228 chars, so the line buffer must clear that. */
#define XCORE_LOG_LINE_MAX    256

typedef struct
{
    XcoreCmdMsg buf[XCORE_CMD_QUEUE_LEN];
    uint32      head;                    /* writer: CPU2 */
    uint32      tail;                    /* reader: CPU0 */
} CmdQueue;

/* One spinlock for all blocks; every critical section copies a few bytes only.
 * IfxCpu_acquireMutex() is a non-blocking cmpAndSwap, so spin here. */
static IfxCpu_mutexLock g_lock;

/* CPU0 -> CPU1 motor targets; seq counts every publish so CPU1 can detect loss */
typedef struct
{
    sint16  left;                        /* -1000..+1000 */
    sint16  right;
    boolean estop;
    uint32  seq;
} MotorTarget;

static MotorTarget g_motorTarget;

/* CPU1 -> telemetry: applied (post algorithm) side speeds */
typedef struct
{
    sint16 left;
    sint16 right;
} MotorStatus;

static MotorStatus g_motorStatus;

/* CPU1 -> telemetry: measured side speeds from the Hall encoders, percent
 * domain for the demo status and physical units for the SF telemetry */
static XcoreEncoder g_encoderStatus;
static uint32 g_encoderSeq;
static FusionTof g_tof;
static FusionOutput g_fusion;
static boolean g_lockIrq[3];

/* CPU2 -> CPU1 fast e-stop bypass (cleared by CPU0 on fault clear/reset) */
static volatile boolean g_estopReq;

/* CPU0 -> CPU1 bench direction-calibration request, test-and-clear consumed
 * by CPU1 so repeated 0x70 commands while a run is in flight do not queue up */
static boolean g_calibReq;

/* CPU1 -> CPU0 calibration result, one block per run (doc 34 SS3.2) */
static XcoreCalibResult g_calibResult;

/* CPU0 -> CPU1 per-motor jog duties + change counter (doc 34 SS9.3) */
static sint16  g_jogDuty[CALIB_REC_WHEELS];
static uint32  g_jogSeq;

/* CPU0 -> CPU1/CPU2 live calibration record (doc 34 SS8/SS9.2) */
static XcoreRecordLive g_recordLive;

/* CPU0 -> CPU2 event frame outbox (doc 34 SS9.4); same ring discipline as the
 * command queue, head written by CPU0, tail by CPU2. */
static XcoreEvtFrame g_evtQueue[XCORE_EVT_QUEUE_LEN];
static uint32        g_evtHead;                /* writer: CPU0 */
static uint32        g_evtTail;                /* reader: CPU2 */

/* CPU1 -> telemetry: battery VIN in mV (bsp/adc EMA-filtered). Single writer
 * CPU1, readers CPU0/CPU2; 0 until the first sample lands. */
static uint16 g_battMv;

/* CPU1 -> CPU0/CPU2: last converted IMU sample (bsp/imu). seq lives outside
 * the struct so the writer cannot forget to bump it. */
static XcoreImu g_imu;
static uint32   g_imuSeq;

/* CPU0 -> CPU2 robot status mirror */
static ProtocolStatus g_status;

/* CPU2 -> CPU0 command queue */
static CmdQueue g_cmdQueue;

/* Single-producer-at-a-time log ring: producers copy one whole line under the
 * lock and advance the write index only afterwards, so CPU0 drains without a
 * lock and never sees a half-written line. */
static char     g_logRing[XCORE_LOG_RING_SIZE];
static uint32   g_logWr;                 /* producers: CPU1/CPU2 */
static uint32   g_logRd;                 /* consumer: CPU0 */

static void XCORE_lock(void)
{
    /* A higher-priority CPU0 task must not spin on its preempted owner.
     * Preserve each core's interrupt state across the bounded RAM copy. */
    boolean enabled = IfxCpu_disableInterrupts();
    uint32 core = (uint32)IfxCpu_getCoreId();
    g_lockIrq[core] = enabled;
    while (!IfxCpu_acquireMutex(&g_lock))
    {
    }
}

static void XCORE_unlock(void)
{
    boolean enabled = g_lockIrq[(uint32)IfxCpu_getCoreId()];
    IfxCpu_releaseMutex(&g_lock);
    IfxCpu_restoreInterrupts(enabled);
}

void XCORE_init(void)
{
    memset((void *)&g_motorTarget, 0, sizeof(g_motorTarget));
    memset((void *)&g_motorStatus, 0, sizeof(g_motorStatus));
    memset((void *)&g_encoderStatus, 0, sizeof(g_encoderStatus));
    memset((void *)&g_status, 0, sizeof(g_status));
    memset((void *)&g_cmdQueue, 0, sizeof(g_cmdQueue));
    memset((void *)g_logRing, 0, sizeof(g_logRing));
    memset((void *)&g_calibResult, 0, sizeof(g_calibResult));
    memset((void *)g_jogDuty, 0, sizeof(g_jogDuty));
    memset((void *)&g_recordLive, 0, sizeof(g_recordLive));
    memset((void *)g_evtQueue, 0, sizeof(g_evtQueue));
    g_lock     = 0;
    g_estopReq = FALSE;
    g_calibReq = FALSE;
    g_battMv   = 0u;
    memset((void *)&g_imu, 0, sizeof(g_imu));
    g_imuSeq   = 0u;
    g_encoderSeq = 0u;
    memset(&g_tof, 0, sizeof(g_tof));
    memset(&g_fusion, 0, sizeof(g_fusion));
    g_logWr    = 0;
    g_logRd    = 0;
    g_jogSeq   = 0u;
    g_evtHead  = 0u;
    g_evtTail  = 0u;
    __dsync();
}

void XCORE_motorSetTarget(sint16 left, sint16 right, boolean estop)
{
    XCORE_lock();
    g_motorTarget.left  = left;
    g_motorTarget.right = right;
    g_motorTarget.estop = estop;
    g_motorTarget.seq++;
    __dsync();
    XCORE_unlock();
}

uint32 XCORE_motorGetTarget(sint16 *left, sint16 *right, boolean *estop)
{
    uint32 seq;

    XCORE_lock();
    *left  = g_motorTarget.left;
    *right = g_motorTarget.right;
    *estop = g_motorTarget.estop;
    seq    = g_motorTarget.seq;
    XCORE_unlock();
    return seq;
}

void XCORE_motorStatusSet(sint16 left, sint16 right)
{
    XCORE_lock();
    g_motorStatus.left  = left;
    g_motorStatus.right = right;
    __dsync();
    XCORE_unlock();
}

void XCORE_motorStatusGet(sint16 *left, sint16 *right)
{
    XCORE_lock();
    *left  = g_motorStatus.left;
    *right = g_motorStatus.right;
    XCORE_unlock();
}

void XCORE_encoderPublish(const XcoreEncoder *enc)
{
    if (enc == NULL_PTR)
    {
        return;
    }

    XCORE_lock();
    g_encoderStatus = *enc;
    g_encoderStatus.seq = ++g_encoderSeq;
    __dsync();
    XCORE_unlock();
}

void XCORE_encoderRead(XcoreEncoder *enc)
{
    XCORE_lock();
    *enc = g_encoderStatus;
    XCORE_unlock();
}

boolean XCORE_encoderIsAlive(void)
{
    boolean alive;

    XCORE_lock();
    alive = g_encoderStatus.alive;
    XCORE_unlock();
    return alive;
}

void XCORE_estopRequest(void)
{
    g_estopReq = TRUE;
    __dsync();
}

void XCORE_estopClear(void)
{
    g_estopReq = FALSE;
    __dsync();
}

boolean XCORE_estopIsActive(void)
{
    return g_estopReq;
}

void XCORE_dirCalibRequest(void)
{
    XCORE_lock();
    g_calibReq = TRUE;
    __dsync();
    XCORE_unlock();
}

boolean XCORE_dirCalibConsume(void)
{
    boolean req;

    XCORE_lock();
    req        = g_calibReq;
    g_calibReq = FALSE;
    XCORE_unlock();
    return req;
}

void XCORE_calibResultPublish(const XcoreCalibResult *res)
{
    if (res == NULL_PTR)
    {
        return;
    }

    XCORE_lock();
    g_calibResult           = *res;
    g_calibResult.pending   = TRUE;
    __dsync();
    XCORE_unlock();
}

boolean XCORE_calibResultTake(XcoreCalibResult *res)
{
    boolean had;

    XCORE_lock();
    had = g_calibResult.pending;
    if (had != FALSE)
    {
        *res = g_calibResult;
        g_calibResult.pending = FALSE;
    }
    XCORE_unlock();
    return had;
}

void XCORE_jogSet(uint8 motor, sint16 duty)
{
    if (motor >= CALIB_REC_WHEELS)
    {
        return;
    }

    XCORE_lock();
    g_jogDuty[motor] = duty;
    g_jogSeq++;
    __dsync();
    XCORE_unlock();
}

uint32 XCORE_jogGet(XcoreJog *jog)
{
    uint32 seq;
    uint8  i;

    XCORE_lock();
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        jog->duty[i] = g_jogDuty[i];
    }
    seq         = g_jogSeq;
    jog->jogSeq = seq;
    XCORE_unlock();
    return seq;
}

void XCORE_jogClear(void)
{
    uint8 i;

    XCORE_lock();
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        g_jogDuty[i] = 0;
    }
    __dsync();
    XCORE_unlock();
}

void XCORE_recordGet(XcoreRecordLive *out)
{
    XCORE_lock();
    *out = g_recordLive;
    XCORE_unlock();
}

void XCORE_recordSet(const CalibRecord *rec)
{
    if (rec == NULL_PTR)
    {
        return;
    }

    XCORE_lock();
    g_recordLive.rec     = *rec;
    g_recordLive.version = (uint8)(g_recordLive.version + 1u);
    __dsync();
    XCORE_unlock();
}

boolean XCORE_evtPush(const XcoreEvtFrame *frame)
{
    boolean ok = FALSE;

    if ((frame == NULL_PTR) || (frame->len > XCORE_EVT_MAX_PAYLOAD))
    {
        return FALSE;
    }

    XCORE_lock();
    if ((g_evtHead - g_evtTail) < XCORE_EVT_QUEUE_LEN)
    {
        g_evtQueue[g_evtHead % XCORE_EVT_QUEUE_LEN] = *frame;
        g_evtHead++;
        __dsync();
        ok = TRUE;
    }
    XCORE_unlock();
    return ok;
}

boolean XCORE_evtPeek(XcoreEvtFrame *frame)
{
    boolean had = FALSE;

    XCORE_lock();
    if (g_evtHead != g_evtTail)
    {
        *frame = g_evtQueue[g_evtTail % XCORE_EVT_QUEUE_LEN];
        had    = TRUE;
    }
    XCORE_unlock();
    return had;
}

void XCORE_evtPop(void)
{
    XCORE_lock();
    if (g_evtHead != g_evtTail)
    {
        g_evtTail++;
    }
    XCORE_unlock();
}

void XCORE_battSetMv(uint16 mv)
{
    XCORE_lock();
    g_battMv = mv;
    __dsync();
    XCORE_unlock();
}

uint16 XCORE_battGetMv(void)
{
    uint16 mv;

    XCORE_lock();
    mv = g_battMv;
    XCORE_unlock();
    return mv;
}

void XCORE_imuPublish(const XcoreImu *imu)
{
    if (imu == NULL_PTR)
    {
        return;
    }
    XCORE_lock();
    g_imu = *imu;
    g_imuSeq++;
    g_imu.seq = g_imuSeq;
    __dsync();
    XCORE_unlock();
}

void XCORE_imuRead(XcoreImu *imu)
{
    if (imu == NULL_PTR)
    {
        return;
    }
    XCORE_lock();
    *imu = g_imu;
    XCORE_unlock();
}

void XCORE_tofPublish(const FusionTof *tof)
{
    if (!tof) return;
    XCORE_lock(); g_tof = *tof; __dsync(); XCORE_unlock();
}
void XCORE_tofRead(FusionTof *tof)
{
    if (!tof) return;
    XCORE_lock(); *tof = g_tof; XCORE_unlock();
}
void XCORE_fusionPublish(const FusionOutput *out)
{
    if (!out) return;
    XCORE_lock(); g_fusion = *out; __dsync(); XCORE_unlock();
}
void XCORE_fusionRead(FusionOutput *out)
{
    if (!out) return;
    XCORE_lock(); *out = g_fusion; XCORE_unlock();
}

void XCORE_statusPublish(const ProtocolStatus *status)
{
    XCORE_lock();
    g_status = *status;
    __dsync();
    XCORE_unlock();
}

void XCORE_statusGet(ProtocolStatus *status)
{
    XCORE_lock();
    *status = g_status;
    XCORE_unlock();
}

boolean XCORE_cmdPush(const XcoreCmdMsg *msg)
{
    boolean ok = FALSE;

    XCORE_lock();
    if ((g_cmdQueue.head - g_cmdQueue.tail) < XCORE_CMD_QUEUE_LEN)
    {
        g_cmdQueue.buf[g_cmdQueue.head % XCORE_CMD_QUEUE_LEN] = *msg;
        g_cmdQueue.head++;
        __dsync();
        ok = TRUE;
    }
    XCORE_unlock();
    return ok;
}

boolean XCORE_cmdPushLatest(const XcoreCmdMsg *msg)
{
    boolean ok = FALSE;
    uint32  idx;

    XCORE_lock();
    /* Newest-wins: overwrite the most recently queued message carrying the
     * same cmd byte instead of appending. For a periodic state command (the
     * 30 Hz SET_SPEED joystick/heartbeat stream) every queued copy older than
     * the one arriving now is superseded the moment it exists, so keeping any
     * of them only occupies depth - the burst of ~100 stale copies a link pump
     * stall produces must collapse into one entry, not into 100 rejections.
     *
     * The newest match is replaced, not the oldest: with [S1, S2] queued and
     * S3 arriving, replacing S2 leaves [S1, S3] so CPU0 still applies S3 last;
     * replacing S1 would leave [S3, S2] and the stale S2 would win. Commands
     * of any other cmd byte keep their relative order, so a STOP queued after
     * the stream still stops after it. */
    for (idx = g_cmdQueue.head; idx != g_cmdQueue.tail;)
    {
        idx--;
        if (g_cmdQueue.buf[idx % XCORE_CMD_QUEUE_LEN].cmd == msg->cmd)
        {
            g_cmdQueue.buf[idx % XCORE_CMD_QUEUE_LEN] = *msg;
            __dsync();
            ok = TRUE;
            break;
        }
    }
    if (ok == FALSE)
    {
        if ((g_cmdQueue.head - g_cmdQueue.tail) < XCORE_CMD_QUEUE_LEN)
        {
            g_cmdQueue.buf[g_cmdQueue.head % XCORE_CMD_QUEUE_LEN] = *msg;
            g_cmdQueue.head++;
            __dsync();
            ok = TRUE;
        }
    }
    XCORE_unlock();
    return ok;
}

boolean XCORE_cmdPop(XcoreCmdMsg *msg)
{
    boolean ok = FALSE;

    XCORE_lock();
    if (g_cmdQueue.head != g_cmdQueue.tail)
    {
        *msg = g_cmdQueue.buf[g_cmdQueue.tail % XCORE_CMD_QUEUE_LEN];
        g_cmdQueue.tail++;
        ok = TRUE;
    }
    XCORE_unlock();
    return ok;
}

void XCORE_log(const char *s)
{
    uint32 len = strlen(s);

    if (len > XCORE_LOG_LINE_MAX)
    {
        len = XCORE_LOG_LINE_MAX;
    }

    XCORE_lock();
    /* Drop the whole line when the ring is nearly full; partial lines would
     * desynchronize the drain side. */
    if ((XCORE_LOG_RING_SIZE - (g_logWr - g_logRd)) > (len + 1))
    {
        uint32 i;

        for (i = 0; i < len; i++)
        {
            g_logRing[g_logWr % XCORE_LOG_RING_SIZE] = s[i];
            g_logWr++;
        }
        g_logRing[g_logWr % XCORE_LOG_RING_SIZE] = '\n';
        g_logWr++;
        __dsync();
    }
    XCORE_unlock();
}

void XCORE_logln(const char *s)
{
    XCORE_log(s);
}

/* uint32 -> decimal, right-aligned into the tail of buf. Returns a pointer to
 * the first digit; never writes a terminator, the caller owns buf. A u32 is at
 * most 10 digits, so a 10 byte scratch is always enough. */
static char *XCORE_u32ToDec(uint32 v, char *end)
{
    char *p = end;

    do
    {
        *(--p) = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);

    return p;
}

void XCORE_logu(const char *label, const uint32 *vals, uint8 n)
{
    char   line[XCORE_LOG_LINE_MAX + 1];
    char   dec[10];
    uint32 idx = 0u;
    uint8  v;

    if (n > XCORE_LOG_MAX_VALS)
    {
        n = XCORE_LOG_MAX_VALS;
    }

    /* Copy the label, leaving room for at least one full value group and the
     * terminator so a long label cannot push a digit past the buffer. */
    if (label != NULL_PTR)
    {
        while ((label[idx] != '\0') && (idx < (uint32)(XCORE_LOG_LINE_MAX - 12)))
        {
            line[idx] = label[idx];
            idx++;
        }
    }

    for (v = 0u; v < n; v++)
    {
        const char *digits = XCORE_u32ToDec((vals != NULL_PTR) ? vals[v] : 0u,
                                            &dec[sizeof(dec)]);
        const char *d;

        if (idx >= (uint32)(XCORE_LOG_LINE_MAX - 11))
        {
            break;      /* no room for another " <=10 digits" group */
        }
        line[idx++] = ' ';
        for (d = digits; d != &dec[sizeof(dec)]; d++)
        {
            line[idx++] = *d;
        }
    }

    line[idx] = '\0';
    XCORE_log(line);
}

/* Signed twin of XCORE_logu for values that are naturally negative (wheel
 * speeds): same line format, each value in decimal with a '-' when negative. */
void XCORE_logi(const char *label, const sint32 *vals, uint8 n)
{
    char   line[XCORE_LOG_LINE_MAX + 1];
    char   dec[10];
    uint32 idx = 0u;
    uint8  v;

    if (n > XCORE_LOG_MAX_VALS)
    {
        n = XCORE_LOG_MAX_VALS;
    }

    /* Copy the label, leaving room for at least one full value group ("- " and
     * 10 digits) and the terminator, exactly like XCORE_logu. */
    if (label != NULL_PTR)
    {
        while ((label[idx] != '\0') && (idx < (uint32)(XCORE_LOG_LINE_MAX - 13)))
        {
            line[idx] = label[idx];
            idx++;
        }
    }

    for (v = 0u; v < n; v++)
    {
        sint32      value = (vals != NULL_PTR) ? vals[v] : 0;
        uint32      mag;
        const char *digits;
        const char *d;

        if (idx >= (uint32)(XCORE_LOG_LINE_MAX - 12))
        {
            break;      /* no room for another " <=10 digits+sign" group */
        }
        line[idx++] = ' ';
        if (value < 0)
        {
            line[idx++] = '-';
            /* Magnitude in uint32: -(INT32_MIN+1) is the last value that fits
             * a sint32, so the +1 happens only after the widen. */
            mag = (uint32)(-(value + 1)) + 1u;
        }
        else
        {
            mag = (uint32)value;
        }
        digits = XCORE_u32ToDec(mag, &dec[sizeof(dec)]);
        for (d = digits; d != &dec[sizeof(dec)]; d++)
        {
            line[idx++] = *d;
        }
    }

    line[idx] = '\0';
    XCORE_log(line);
}

void XCORE_logService(void)
{
    char line[XCORE_LOG_LINE_MAX + 1];

    /* Exactly one line per call. This runs inside CPU0's 10 ms control task,
     * and UART_println blocks on the ASCLIN0 FIFO at line rate (~87 us/byte
     * at 115200): draining the whole ring in one go parked the task for the
     * ~180 ms a full 2 KB backlog takes to shift out, which starved the
     * command queue drain in the same loop and turned any log burst into a
     * control stall. One line bounds the block at one line's transmit time
     * (worst case XCORE_LOG_LINE_MAX bytes ~= 22 ms) while still draining
     * 100 lines/s - 25x the steady-state producer rate of ~4 lines/s
     * (LINKDBG 2/s + SPD 1/s + SRV 1/s). A larger backlog then takes seconds
     * to clear, but it clears without ever touching the control period's
     * budget, and producers drop whole lines on a full ring by design. */
    if (g_logRd != g_logWr)
    {
        uint32 idx = 0;

        while ((g_logRd != g_logWr) && (idx < XCORE_LOG_LINE_MAX))
        {
            char c = g_logRing[g_logRd % XCORE_LOG_RING_SIZE];

            g_logRd++;
            if (c == '\n')
            {
                break;
            }
            line[idx++] = c;
        }
        line[idx] = '\0';
        if (idx > 0)
        {
            UART_println(line);
        }
    }
}
