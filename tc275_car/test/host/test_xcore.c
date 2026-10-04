/*
 * test_xcore.c - host unit tests for the cross-core command queue and log ring
 *
 * mw/xcore/xcore.c runs on all three cores behind a hardware spinlock, but
 * the discipline inside the lock is plain index arithmetic: FIFO order, queue
 * capacity, newest-wins coalescing and the bounded non-blocking log pump.
 * Those are exactly the parts the 2026-10-03 "LINK cmdq full" field failure
 * lived in - plus the 2026-10-04 finding that the old one-line-per-call drain
 * still parked the 10 ms control task for up to one line's transmit time -
 * so they get compiled and asserted here instead of trusted.
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
#define TEST_TX_FIFO   256              /* mirrors ASC_TX_BUFFER_SIZE in bsp/uart.c */

static char g_uartLines[64][TEST_LINE_MAX];
static int  g_uartCount;

/* Simulated ASCLIN0 in three layers, mirroring the target data path:
 * UART_printTry queues bytes (CRLF-expanded) into the driver's software TX
 * FIFO without ever waiting; test_uartDrain plays the TX ISR moving FIFO
 * bytes onto the wire; test_uartCollect re-splits the wire stream into
 * terminal lines on '\n'. A wire byte order violation or a split CRLF pair
 * shows up here exactly as it would on a terminal. */
static uint8 g_txFifo[TEST_TX_FIFO];
static int   g_txUsed;
static char  g_wire[16384];
static int   g_wireLen;

uint32 UART_printTry(const char *data, uint32 len)
{
    uint32 accepted = 0;

    while (accepted < len)
    {
        char   c    = data[accepted];
        uint32 need = (c == '\n') ? 2u : 1u;    /* '\n' goes out as CRLF */

        if ((uint32)(TEST_TX_FIFO - g_txUsed) < need)
        {
            break;                      /* both bytes or none, like the real one */
        }
        if (c == '\n')
        {
            g_txFifo[g_txUsed++] = (uint8)'\r';
        }
        g_txFifo[g_txUsed++] = (uint8)c;
        accepted++;
    }

    return accepted;
}

static void test_uartReset(void)
{
    g_txUsed = 0;
    g_wireLen = 0;
    g_uartCount = 0;
    memset(g_txFifo, 0, sizeof(g_txFifo));
    memset(g_wire, 0, sizeof(g_wire));
    memset(g_uartLines, 0, sizeof(g_uartLines));
}

/* TX ISR stand-in: shift n FIFO bytes onto the wire. */
static void test_uartDrain(int n)
{
    while ((n > 0) && (g_txUsed > 0))
    {
        g_wire[g_wireLen++] = (char)g_txFifo[0];
        memmove(g_txFifo, g_txFifo + 1, (size_t)(g_txUsed - 1));
        g_txUsed--;
        n--;
    }
}

static void test_uartDrainAll(void)
{
    test_uartDrain(g_txUsed);
}

/* Terminal stand-in: re-split the wire stream into lines ('\r' stripped,
 * like a terminal that shows the text but not the control bytes). */
static void test_uartCollect(void)
{
    int start = 0;
    int i;

    g_uartCount = 0;
    for (i = 0; i <= g_wireLen; i++)
    {
        if ((i == g_wireLen) || (g_wire[i] == '\n'))
        {
            int end = i;

            if ((end > start) && (g_wire[end - 1] == '\r'))
            {
                end--;                  /* CRLF: drop the CR like a terminal */
            }
            if ((end > start) &&
                (g_uartCount < (int)(sizeof(g_uartLines) / sizeof(g_uartLines[0]))))
            {
                int len = end - start;

                strncpy(g_uartLines[g_uartCount], &g_wire[start],
                        (size_t)len);
                g_uartLines[g_uartCount][len] = '\0';
                g_uartCount++;
            }
            start = i + 1;
        }
    }
}

/* "FIFO always keeps up" view: service, shift everything out, re-split.
 * Matches how the real system looks whenever the producers stay below the
 * 115200 baud line rate between two control ticks. */
static void test_serviceFast(void)
{
    XCORE_logService();
    test_uartDrainAll();
    test_uartCollect();
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

/* ---- log pump: bounded, non-blocking, order-preserving ----------------------
 *
 * XCORE_logService runs in CPU0's 10 ms control task next to the watchdog
 * feed. The old one-line-per-call drain blocked that task for up to one
 * line's transmit time (~22 ms for 256 B at 115200 - more than two control
 * periods) whenever the ASCLIN0 FIFO was full. The pump must therefore only
 * ever queue bytes the software TX FIFO accepts immediately, and resume
 * exactly where it stopped on a later call. */
static void test_logservice_pump_bounded_by_fifo(void)
{
    char filler[400];
    int  guard;

    XCORE_init();
    test_uartReset();
    memset(filler, 'x', sizeof(filler));

    /* Software TX FIFO filled from elsewhere (echo task style): the pump
     * must move nothing and - the point of the fix - not wait for the line. */
    CHECK_EQ(UART_printTry(filler, sizeof(filler)), (uint32)TEST_TX_FIFO);
    XCORE_logln("hello");
    XCORE_logService();
    CHECK_EQ(g_txUsed, TEST_TX_FIFO);           /* FIFO untouched          */

    /* One free slot: exactly one byte moves per call, never a spin. */
    test_uartDrain(1);
    XCORE_logService();
    CHECK_EQ(g_txUsed, TEST_TX_FIFO);           /* 'h' refilled the slot   */

    /* Draining in bursts clears the rest without ever exceeding the FIFO. */
    while (g_txUsed > 0)
    {
        test_uartDrain(3);
        XCORE_logService();
        CHECK(g_txUsed <= TEST_TX_FIFO);
    }
    for (guard = 0; guard < 10; guard++)
    {
        XCORE_logService();                     /* ring dry: no movement   */
        CHECK_EQ(g_txUsed, 0);
    }
    test_uartCollect();
    /* The unterminated filler run and "hello" form one physical line that
     * the trailing CRLF closes: 256 intact filler bytes, then hello. */
    CHECK_EQ(g_uartCount, 1);
    CHECK_EQ(strlen(g_uartLines[0]), (size_t)(TEST_TX_FIFO + 5));
    CHECK(strncmp(g_uartLines[0], "xxxx", 4) == 0);
    CHECK(strcmp(&g_uartLines[0][TEST_TX_FIFO], "hello") == 0);
    CHECK(strcmp(&g_wire[TEST_TX_FIFO], "hello\r\n") == 0);
}

static void test_logservice_pump_resumes_in_order(void)
{
    char line[16];
    char expect[16];
    int  i;

    XCORE_init();
    test_uartReset();

    for (i = 0; i < 40; i++)                    /* 40 x "line 000\n" = 320 B:
                                                 * more than the 256 B FIFO */
    {
        sprintf(line, "line %03d", i);
        XCORE_logln(line);
    }

    /* One call may only queue what the FIFO accepts; "line %03d\n" is 9 B
     * per line so no CRLF sits on the 256 B boundary: exactly full. */
    XCORE_logService();
    CHECK_EQ(g_txUsed, TEST_TX_FIFO);

    while (g_txUsed > 0)
    {
        test_uartDrain(7);                      /* small "line rate" bursts */
        XCORE_logService();
    }
    test_uartCollect();

    CHECK_EQ(g_wireLen, 40 * (8 + 2));          /* 8 chars + CRLF per line  */
    CHECK_EQ(g_uartCount, 40);
    for (i = 0; i < 40; i++)
    {
        sprintf(expect, "line %03d", i);
        CHECK(strcmp(g_uartLines[i], expect) == 0);
    }
}

static void test_logservice_pump_newline_never_split(void)
{
    char filler[255];

    XCORE_init();
    test_uartReset();
    memset(filler, 'x', sizeof(filler));

    /* One free slot and a '\n' at the ring head: CRLF needs two slots, so
     * the newline must stay queued - neither consumed as a lone CR nor
     * passed over - or the terminal would see broken framing. */
    CHECK_EQ(UART_printTry(filler, sizeof(filler)), (uint32)sizeof(filler));
    XCORE_logln("");                        /* ring: "\n" only           */
    XCORE_logService();
    CHECK_EQ(g_txUsed, (int)sizeof(filler));/* nothing consumed          */

    test_uartDrain(1);
    XCORE_logService();                     /* both bytes fit now        */
    CHECK_EQ(g_txUsed, TEST_TX_FIFO);
    test_uartDrainAll();
    CHECK((g_wireLen == (int)sizeof(filler) + 2) &&
          (g_wire[g_wireLen - 2] == '\r') &&
          (g_wire[g_wireLen - 1] == '\n'));
    test_uartCollect();
    CHECK_EQ(g_uartCount, 1);               /* CRLF closed the filler    */
    CHECK_EQ(strlen(g_uartLines[0]), sizeof(filler));
}

/* Whole backlog with the FIFO keeping up: everything drains in one call,
 * lines in FIFO order, wire bytes exactly ring order. */
static void test_logservice_drains_all_when_fifo_keeps_up(void)
{
    XCORE_init();
    test_uartReset();

    XCORE_logln("line one");
    XCORE_logln("line two");
    XCORE_logln("line three");

    test_serviceFast();
    CHECK_EQ(g_uartCount, 3);
    CHECK(strcmp(g_uartLines[0], "line one") == 0);
    CHECK(strcmp(g_uartLines[1], "line two") == 0);
    CHECK(strcmp(g_uartLines[2], "line three") == 0);

    test_serviceFast();                         /* ring empty: no output   */
    CHECK_EQ(g_uartCount, 3);
    CHECK_EQ(g_txUsed, 0);
}

static void test_logu_line_format(void)
{
    uint32 vals[2];

    XCORE_init();
    test_uartReset();

    vals[0] = 1u;
    vals[1] = 42u;
    XCORE_logu("LINK cmdq full x", vals, 2u);
    test_serviceFast();

    CHECK_EQ(g_uartCount, 1);
    CHECK(strcmp(g_uartLines[0], "LINK cmdq full x 1 42") == 0);
}

/* IMU block (doc 35): single-writer mailbox whose seq is bumped by the
 * publisher, not the caller - the zero state after XCORE_init means "never
 * published", so consumers watch seq for freshness, never the data. */
static void test_imu_block(void)
{
    XcoreImu pub;
    XcoreImu out;

    XCORE_init();

    XCORE_imuRead(&out);
    CHECK_EQ(out.seq, 0u);                          /* never published      */
    CHECK(out.alive == FALSE);

    memset(&pub, 0, sizeof(pub));
    pub.alive         = TRUE;
    pub.whoAmI        = 0x71;
    pub.accMilliG[0]  = -999;
    pub.gyroMilliDps[2] = 17500;
    pub.tempCentiC    = 2560;
    pub.drdyCount     = 5u;
    XCORE_imuPublish(&pub);

    XCORE_imuRead(&out);
    CHECK_EQ(out.seq, 1u);                          /* first publish = 1    */
    CHECK(out.alive == TRUE);
    CHECK_EQ(out.whoAmI, 0x71);
    CHECK_EQ(out.accMilliG[0], -999);
    CHECK_EQ(out.accMilliG[1], 0);
    CHECK_EQ(out.gyroMilliDps[2], 17500);
    CHECK_EQ(out.tempCentiC, 2560);
    CHECK_EQ(out.drdyCount, 5u);

    /* the caller's copy never gains the seq - the block owns the counter */
    CHECK_EQ(pub.seq, 0u);

    pub.accMilliG[0] = 123;
    XCORE_imuPublish(&pub);
    XCORE_imuRead(&out);
    CHECK_EQ(out.seq, 2u);                          /* monotonic            */
    CHECK_EQ(out.accMilliG[0], 123);

    XCORE_imuPublish(NULL_PTR);                     /* no crash, no bump    */
    XCORE_imuRead(&out);
    CHECK_EQ(out.seq, 2u);
}

static void test_sensor_snapshots(void)
{
    FusionTof tof, got;
    FusionOutput out, read;
    XcoreEncoder enc;
    XCORE_init();
    memset(&tof, 0, sizeof(tof)); memset(&out, 0, sizeof(out));
    memset(&enc, 0, sizeof(enc));
    enc.alive = 1; enc.edgeAgeMs[0] = 7u; enc.edgeAgeMs[1] = 65535u;
    XCORE_encoderPublish(&enc); XCORE_encoderRead(&enc);
    CHECK_EQ(enc.seq, 1u);
    CHECK_EQ(enc.edgeAgeMs[0], 7u);                 /* health rides along   */
    CHECK_EQ(enc.edgeAgeMs[1], 65535u);
    XCORE_encoderPublish(&enc); XCORE_encoderRead(&enc);
    CHECK_EQ(enc.seq, 2u);
    tof.seq=9; tof.stampMs=123; tof.zones=64; tof.alive=1;
    tof.distanceMm[63]=321; tof.status[63]=9; tof.targets[63]=1;
    XCORE_tofPublish(&tof); XCORE_tofRead(&got);
    CHECK_EQ(got.seq,9u); CHECK_EQ(got.stampMs,123u);
    CHECK_EQ(got.distanceMm[63],321); CHECK_EQ(got.targets[63],1);
    out.effective[0]=-250; out.flags=FUSION_IMU_OK; out.brake=1;
    XCORE_fusionPublish(&out); XCORE_fusionRead(&read);
    CHECK_EQ(read.effective[0],-250); CHECK_EQ(read.flags,FUSION_IMU_OK);
    CHECK_EQ(read.brake,1);
    XCORE_tofPublish(NULL_PTR); XCORE_fusionPublish(NULL_PTR);
    XCORE_tofRead(NULL_PTR); XCORE_fusionRead(NULL_PTR);
    XCORE_init(); XCORE_tofRead(&got); XCORE_fusionRead(&read);
    XCORE_encoderRead(&enc);
    CHECK_EQ(got.seq,0u); CHECK_EQ(read.flags,0); CHECK_EQ(enc.seq,0u);
}

static void test_named_logs(void)
{
    XcoreLogGate gate = {0};
    XcoreLogField many[20];
    char longText[180];
    int i, found = 0;
    XCORE_init(); test_uartReset();
    XCORE_LOG_FIELDS("[TEST]", XL_U("total", 0xFFFFFFFFu),
        XL_I("signed", (-2147483647 - 1)), XL_H("register", 0xFFFFFFFFu),
        XL_S("state", "ready"), XL_S("missing", NULL_PTR));
    test_serviceFast();
    CHECK_EQ(g_uartCount, 1);
    CHECK(strcmp(g_uartLines[0], "[TEST] total=4294967295 signed=-2147483648 register=0xFFFFFFFF state=ready missing=unknown") == 0);
    memset(longText, 'x', sizeof(longText)); longText[179] = 0;
    XCORE_LOG_FIELDS("[LONG]", XL_S("text", longText), XL_U("after", 123));
    test_serviceFast();
    CHECK(strstr(g_uartLines[1], "... after=123") != NULL);
    for (i = 0; i < 20; i++) {
        many[i].name = "counter_total"; many[i].number = 4294967295u;
        many[i].text = NULL_PTR; many[i].kind = 0u;
    }
    test_uartReset();
    XCORE_logFields("[SPLIT]", many, 20u);
    for (i=0; i<10; i++) test_serviceFast();
    CHECK(g_uartCount >= 2);
    for (i=0; i<g_uartCount; i++) {
        const char *cursor = g_uartLines[i];
        CHECK(strlen(cursor) <= 256u);
        CHECK(strncmp(cursor, "[SPLIT] ", 8u) == 0);
        while ((cursor = strstr(cursor, "counter_total=4294967295")) != NULL) {
            found++; cursor += strlen("counter_total=4294967295");
        }
    }
    CHECK_EQ(found, 20);
    CHECK_EQ(XCORE_logDropped(), 0);
    /* Must exceed the 2048 B ring even when it starts empty. */
    for (i=0; i<300; i++) XCORE_logln("overflow");
    CHECK(XCORE_logDropped() > 0);
    XCORE_init(); CHECK_EQ(XCORE_logDropped(), 0);
    CHECK(XCORE_logDue(&gate, 100u, 1u, 5000u, 1000u, FALSE));
    CHECK(!XCORE_logDue(&gate, 200u, 1u, 5000u, 1000u, TRUE));
    CHECK(!XCORE_logDue(&gate, 200u, 2u, 5000u, 1000u, FALSE));
    CHECK(XCORE_logDue(&gate, 1100u, 2u, 5000u, 1000u, FALSE));
    CHECK(XCORE_logDue(&gate, 1110u, 3u, 5000u, 1000u, TRUE));
    CHECK(!XCORE_logDue(&gate, 1120u, 3u, 5000u, 1000u, TRUE));
    CHECK(XCORE_logDue(&gate, 6110u, 3u, 5000u, 1000u, FALSE));
    gate.lastMs = 0xFFFFFFF0u;
    CHECK(!XCORE_logDue(&gate, 20u, 3u, 5000u, 1000u, FALSE));
    CHECK(XCORE_logDue(&gate, 5000u, 3u, 5000u, 1000u, FALSE));
}

/* ---- main ------------------------------------------------------------------- */

static void test_diagnostic_channels(void)
{
    XcoreCmdMsg cmd, got;
    XcoreEvtFrame event, read;
    XcoreImu imu;
    FusionTof tof;
    uint32 stamp;
    unsigned i;
    XCORE_init(); memset(&cmd,0,sizeof(cmd)); cmd.cmd=0x53; cmd.len=16;
    for(i=0;i<4;i++) { cmd.data[8]=(uint8)i; CHECK(XCORE_diagCmdPush(&cmd)); }
    CHECK(!XCORE_diagCmdPush(&cmd));
    /* Diagnostic saturation must leave every ordinary command slot available. */
    cmd.cmd=PROTO_CMD_SET_SPEED; CHECK(XCORE_cmdPush(&cmd));
    CHECK(XCORE_cmdPop(&got)); CHECK_EQ(got.cmd,PROTO_CMD_SET_SPEED);
    for(i=0;i<4;i++) {
        CHECK(XCORE_diagCmdPeek(&got)); CHECK_EQ(got.data[8],i);
        CHECK(XCORE_diagCmdPeek(&got)); CHECK_EQ(got.data[8],i); XCORE_diagCmdPop();
    }
    CHECK(!XCORE_diagCmdPeek(&got));
    memset(&event,0,sizeof(event)); event.type=5; event.cid=0x29; event.len=32;
    for(i=0;i<8;i++) { event.payload[0]=(uint8)i; CHECK(XCORE_dataEvtPush(&event)); }
    CHECK(!XCORE_dataEvtPush(&event));
    event.cid=0x28; CHECK(XCORE_evtPush(&event)); CHECK(XCORE_evtPeek(&read));
    CHECK_EQ(read.cid,0x28); XCORE_evtPop();
    for(i=0;i<8;i++) {
        CHECK(XCORE_dataEvtPeek(&read)); CHECK_EQ(read.payload[0],i);
        CHECK(XCORE_dataEvtPeek(&read)); CHECK_EQ(read.payload[0],i); XCORE_dataEvtPop();
    }
    CHECK(!XCORE_dataEvtPeek(&read));
    event.len=33; CHECK(!XCORE_dataEvtPush(&event));
    XCORE_linkPublish(TRUE,1234); CHECK(XCORE_linkRead(&stamp)); CHECK_EQ(stamp,1234);
    XCORE_benchSetActive(TRUE); CHECK(XCORE_benchIsActive());
    XCORE_benchSetActive(FALSE); CHECK(!XCORE_benchIsActive());
    XCORE_dirCalibRequest(); CHECK(XCORE_benchIsActive()); CHECK(XCORE_dirCalibConsume());
    CHECK(!XCORE_benchIsActive());
    memset(&imu,0,sizeof(imu)); imu.stampMs=0xfffffffeu;
    XCORE_imuPublish(&imu); XCORE_imuRead(&imu); CHECK_EQ(imu.stampMs,0xfffffffeu);
    memset(&tof,0,sizeof(tof)); tof.sampleStampMs=9876;
    XCORE_tofPublish(&tof); XCORE_tofRead(&tof); CHECK_EQ(tof.sampleStampMs,9876);
    XCORE_init(); CHECK(!XCORE_linkRead(&stamp)); CHECK_EQ(stamp,0);
    CHECK(!XCORE_benchIsActive()); CHECK(!XCORE_dataEvtPeek(&read)); CHECK(!XCORE_diagCmdPeek(&got));
}

int main(void)
{
    test_cmd_fifo_and_capacity();
    test_cmdpush_latest_replaces();
    test_cmdpush_latest_replaces_newest_of_two();
    test_cmdpush_latest_keeps_order_of_others();
    test_cmdpush_latest_burst_collapses();
    test_cmdpush_latest_no_match_appends();
    test_logservice_pump_bounded_by_fifo();
    test_logservice_pump_resumes_in_order();
    test_logservice_pump_newline_never_split();
    test_logservice_drains_all_when_fifo_keeps_up();
    test_logu_line_format();
    test_imu_block();
    test_sensor_snapshots();
    test_named_logs();
    test_diagnostic_channels();

    printf("%d checks, %d failures\n", g_checks, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
