/*
 * test_xcore.c - host unit tests for the cross-core command queue and log ring
 *
 * mw/xcore/xcore.c runs on all three cores behind a hardware spinlock, but
 * the discipline inside the lock is plain index arithmetic: FIFO order, queue
 * capacity, newest-wins coalescing and the one-line-per-call log drain. Those
 * are exactly the parts the 2026-10-03 "LINK cmdq full" field failure lived
 * in, so they get compiled and asserted here instead of trusted.
 *
 * The hardware stand-ins are minimal on purpose (stub/IfxCpu.h makes the
 * mutex vacuous - single-threaded host - and the UART capture below replaces
 * ASCLIN0), because what must not regress is the queue semantics, not the
 * TriCore atomics.
 *
 * Build & run (MSYS2/MinGW host), same pattern as test_sf.c but with the stub
 * directory FIRST on the include path so bsp/uart.h and IfxCpu.h resolve to
 * the stand-ins instead of the iLLD-dependent originals:
 *   gcc -std=c99 -Wall -Wextra -Werror -O2 -I test/host/stub -I . \
 *       test/host/test_xcore.c mw/xcore/xcore.c \
 *       -o test/host/out/test_xcore && test/host/out/test_xcore
 */
#include <stdio.h>
#include <string.h>

#include "mw/xcore/xcore.h"

static int g_checks;
static int g_failed;

#define CHECK(cond)                                                       \
    do                                                                    \
    {                                                                     \
        g_checks++;                                                       \
        if (!(cond))                                                      \
        {                                                                 \
            g_failed++;                                                   \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                 \
    } while (0)

#define CHECK_EQ(a, b)                                                    \
    do                                                                    \
    {                                                                     \
        long _a = (long)(a), _b = (long)(b);                              \
        g_checks++;                                                       \
        if (_a != _b)                                                     \
        {                                                                 \
            g_failed++;                                                   \
            printf("FAIL %s:%d  %s == %s  (%ld vs %ld)\n",                \
                   __FILE__, __LINE__, #a, #b, _a, _b);                   \
        }                                                                 \
    } while (0)

/* ---- UART capture: what CPU0 would have shipped to ASCLIN0 -------------- */

#define TEST_LINE_MAX  260              /* mirrors xcore.c's XCORE_LOG_LINE_MAX */

static char g_uartLines[64][TEST_LINE_MAX];
static int  g_uartCount;

void UART_println(const char *str)
{
    if (g_uartCount < (int)(sizeof(g_uartLines) / sizeof(g_uartLines[0])))
    {
        strncpy(g_uartLines[g_uartCount], str, sizeof(g_uartLines[0]) - 1u);
        g_uartLines[g_uartCount][sizeof(g_uartLines[0]) - 1u] = '\0';
        g_uartCount++;
    }
}

/* ---- helpers --------------------------------------------------------------- */

static XcoreCmdMsg mkcmd(uint8 cmd, uint8 v)
{
    XcoreCmdMsg m;

    memset(&m, 0, sizeof(m));
    m.cmd     = cmd;
    m.len     = 1u;
    m.data[0] = v;
    return m;
}

/* Pop everything and return the number of messages drained. */
static int drainAll(XcoreCmdMsg *last)
{
    XcoreCmdMsg m;
    int         n = 0;

    while (XCORE_cmdPop(&m) != FALSE)
    {
        n++;
        if (last != NULL)
        {
            *last = m;
        }
    }
    return n;
}

/* ---- command queue: FIFO and capacity -------------------------------------- */

static void test_cmd_fifo_and_capacity(void)
{
    XcoreCmdMsg m;

    XCORE_init();

    CHECK_EQ(XCORE_cmdPush(&(XcoreCmdMsg){ .cmd = 0x01, .len = 0 }), TRUE);
    for (uint8 i = 0; i < 15u; i++)
    {
        XcoreCmdMsg msg = mkcmd(0x30, i);
        CHECK_EQ(XCORE_cmdPush(&msg), TRUE);
    }
    /* 16 slots: the sixteenth push above filled the last one, a 17th fails */
    {
        XcoreCmdMsg msg = mkcmd(0x31, 0);
        CHECK_EQ(XCORE_cmdPush(&msg), FALSE);
    }

    CHECK_EQ(XCORE_cmdPop(&m), TRUE);
    CHECK_EQ(m.cmd, 0x01);                    /* FIFO: first in, first out */
    for (uint8 i = 0; i < 15u; i++)
    {
        CHECK_EQ(XCORE_cmdPop(&m), TRUE);
        CHECK_EQ(m.cmd, 0x30);
        CHECK_EQ(m.data[0], i);
    }
    {
        XcoreCmdMsg none;
        CHECK_EQ(XCORE_cmdPop(&none), FALSE); /* drained */
    }
}

/* ---- command queue: newest-wins coalescing ---------------------------------
 *
 * The 30 Hz SET_SPEED stream is pushed with XCORE_cmdPushLatest: a stale
 * queued copy must be replaced (not duplicated, not rejected), the newest of
 * several matches is the one replaced, and other commands keep their
 * relative order around it. */
static void test_cmdpush_latest_replaces(void)
{
    XcoreCmdMsg m;

    XCORE_init();

    {
        XcoreCmdMsg s1 = mkcmd(PROTO_CMD_SET_SPEED, 10);
        CHECK_EQ(XCORE_cmdPushLatest(&s1), TRUE);
    }
    {
        XcoreCmdMsg s2 = mkcmd(PROTO_CMD_SET_SPEED, 20);
        CHECK_EQ(XCORE_cmdPushLatest(&s2), TRUE);   /* replaces, no append */
    }
    CHECK_EQ(drainAll(&m), 1);                      /* one entry survives   */
    CHECK_EQ(m.cmd, PROTO_CMD_SET_SPEED);
    CHECK_EQ(m.data[0], 20);                        /* carrying the latest  */
}

static void test_cmdpush_latest_replaces_newest_of_two(void)
{
    XcoreCmdMsg m;

    XCORE_init();

    /* Two same-cmd entries can only exist when pushes outpaced the drain:
     * with [S1, S2] queued, S3 must overwrite S2, so the consumer still
     * applies S3's data last. Overwriting S1 instead would leave [S3, S2]
     * and the stale S2 would win - the trap the backwards scan avoids. */
    {
        XcoreCmdMsg s1 = mkcmd(PROTO_CMD_SET_SPEED, 10);
        XcoreCmdMsg s2 = mkcmd(PROTO_CMD_SET_SPEED, 20);
        (void)XCORE_cmdPush(&s1);
        (void)XCORE_cmdPush(&s2);
    }
    {
        XcoreCmdMsg s3 = mkcmd(PROTO_CMD_SET_SPEED, 30);
        CHECK_EQ(XCORE_cmdPushLatest(&s3), TRUE);
    }
    CHECK_EQ(drainAll(&m), 2);
    CHECK_EQ(m.data[0], 30);                        /* last applied = S3    */
}

static void test_cmdpush_latest_keeps_order_of_others(void)
{
    XcoreCmdMsg m;

    XCORE_init();

    {
        XcoreCmdMsg s  = mkcmd(PROTO_CMD_SET_SPEED, 10);
        XcoreCmdMsg st = mkcmd(PROTO_CMD_STOP, 0);
        (void)XCORE_cmdPushLatest(&s);
        (void)XCORE_cmdPush(&st);                   /* STOP after the stream */
    }
    {
        XcoreCmdMsg s = mkcmd(PROTO_CMD_SET_SPEED, 40);
        CHECK_EQ(XCORE_cmdPushLatest(&s), TRUE);
    }
    CHECK_EQ(XCORE_cmdPop(&m), TRUE);
    CHECK_EQ(m.cmd, PROTO_CMD_SET_SPEED);           /* stream still first    */
    CHECK_EQ(m.data[0], 40);
    CHECK_EQ(XCORE_cmdPop(&m), TRUE);
    CHECK_EQ(m.cmd, PROTO_CMD_STOP);                /* stop still last       */
}

static void test_cmdpush_latest_burst_collapses(void)
{
    /* The field failure shape: ~100 superseded stream copies arriving in one
     * burst. Coalescing must absorb all of them without ever reporting a
     * full queue, no matter how slow the consumer is. */
    XcoreCmdMsg m;

    XCORE_init();
    for (uint16 i = 0; i < 1000u; i++)
    {
        XcoreCmdMsg s = mkcmd(PROTO_CMD_SET_SPEED, (uint8)(i & 0xFFu));
        CHECK_EQ(XCORE_cmdPushLatest(&s), TRUE);
    }
    CHECK_EQ(drainAll(&m), 1);
    CHECK_EQ(m.data[0], (1000u - 1u) & 0xFFu);      /* the newest position   */
}

static void test_cmdpush_latest_no_match_appends(void)
{
    XcoreCmdMsg m;

    XCORE_init();
    {
        XcoreCmdMsg st = mkcmd(PROTO_CMD_STOP, 0);
        (void)XCORE_cmdPush(&st);
    }
    {
        XcoreCmdMsg s = mkcmd(PROTO_CMD_SET_SPEED, 7);
        CHECK_EQ(XCORE_cmdPushLatest(&s), TRUE);    /* no match: plain push  */
    }
    CHECK_EQ(XCORE_cmdPop(&m), TRUE);
    CHECK_EQ(m.cmd, PROTO_CMD_STOP);
    CHECK_EQ(XCORE_cmdPop(&m), TRUE);
    CHECK_EQ(m.cmd, PROTO_CMD_SET_SPEED);
    CHECK_EQ(m.data[0], 7);
}

/* ---- log ring: one line per service call -----------------------------------
 *
 * XCORE_logService runs in CPU0's 10 ms control task; draining the whole ring
 * in one call used to park that task for the transmit time of 2 KB (~180 ms
 * at 115200) and starve the command queue drain in the same loop. */
static void test_logservice_one_line_per_call(void)
{
    XCORE_init();
    g_uartCount = 0;

    XCORE_logln("line one");
    XCORE_logln("line two");
    XCORE_logln("line three");

    XCORE_logService();
    CHECK_EQ(g_uartCount, 1);                       /* one line, first out  */
    CHECK(strcmp(g_uartLines[0], "line one") == 0);

    XCORE_logService();
    CHECK_EQ(g_uartCount, 2);
    CHECK(strcmp(g_uartLines[1], "line two") == 0);

    XCORE_logService();
    CHECK_EQ(g_uartCount, 3);

    XCORE_logService();                             /* ring empty: no print */
    CHECK_EQ(g_uartCount, 3);
}

static void test_logu_line_format(void)
{
    uint32 vals[2];

    XCORE_init();
    g_uartCount = 0;

    vals[0] = 1u;
    vals[1] = 42u;
    XCORE_logu("LINK cmdq full x", vals, 2u);
    XCORE_logService();

    CHECK_EQ(g_uartCount, 1);
    CHECK(strcmp(g_uartLines[0], "LINK cmdq full x 1 42") == 0);
}

/* ---- main ------------------------------------------------------------------- */

int main(void)
{
    test_cmd_fifo_and_capacity();
    test_cmdpush_latest_replaces();
    test_cmdpush_latest_replaces_newest_of_two();
    test_cmdpush_latest_keeps_order_of_others();
    test_cmdpush_latest_burst_collapses();
    test_cmdpush_latest_no_match_appends();
    test_logservice_one_line_per_call();
    test_logu_line_format();

    printf("%d checks, %d failures\n", g_checks, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
