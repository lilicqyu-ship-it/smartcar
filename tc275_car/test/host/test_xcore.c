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
    XCORE_encoderPublish(&enc); XCORE_encoderRead(&enc);
    CHECK_EQ(enc.seq, 1u);
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
    XCORE_init(); g_uartCount = 0;
    XCORE_LOG_FIELDS("[TEST]", XL_U("total", 0xFFFFFFFFu),
        XL_I("signed", (-2147483647 - 1)), XL_H("register", 0xFFFFFFFFu),
        XL_S("state", "ready"), XL_S("missing", NULL_PTR));
    XCORE_logService();
    CHECK_EQ(g_uartCount, 1);
    CHECK(strcmp(g_uartLines[0], "[TEST] total=4294967295 signed=-2147483648 register=0xFFFFFFFF state=ready missing=unknown") == 0);
    memset(longText, 'x', sizeof(longText)); longText[179] = 0;
    XCORE_LOG_FIELDS("[LONG]", XL_S("text", longText), XL_U("after", 123));
    XCORE_logService();
    CHECK(strstr(g_uartLines[1], "... after=123") != NULL);
    for (i = 0; i < 20; i++) {
        many[i].name = "counter_total"; many[i].number = 4294967295u;
        many[i].text = NULL_PTR; many[i].kind = 0u;
    }
    g_uartCount = 0;
    XCORE_logFields("[SPLIT]", many, 20u);
    for (i=0; i<10; i++) XCORE_logService();
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
    test_logservice_one_line_per_call();
    test_logu_line_format();
    test_imu_block();
    test_sensor_snapshots();
    test_named_logs();
    test_diagnostic_channels();

    printf("%d checks, %d failures\n", g_checks, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
