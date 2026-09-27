#include "mw/xcore/xcore.h"
#include "bsp/uart.h"
#include "IfxCpu.h"

#include <string.h>

#define XCORE_CMD_QUEUE_LEN   8
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

/* CPU2 -> CPU1 fast e-stop bypass (cleared by CPU0 on fault clear/reset) */
static volatile boolean g_estopReq;

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
    while (!IfxCpu_acquireMutex(&g_lock))
    {
    }
}

static void XCORE_unlock(void)
{
    IfxCpu_releaseMutex(&g_lock);
}

void XCORE_init(void)
{
    memset((void *)&g_motorTarget, 0, sizeof(g_motorTarget));
    memset((void *)&g_motorStatus, 0, sizeof(g_motorStatus));
    memset((void *)&g_encoderStatus, 0, sizeof(g_encoderStatus));
    memset((void *)&g_status, 0, sizeof(g_status));
    memset((void *)&g_cmdQueue, 0, sizeof(g_cmdQueue));
    memset((void *)g_logRing, 0, sizeof(g_logRing));
    g_lock     = 0;
    g_estopReq = FALSE;
    g_logWr    = 0;
    g_logRd    = 0;
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

    while (g_logRd != g_logWr)
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
