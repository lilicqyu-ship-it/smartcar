/* Scheduler regression harness; run with python3 test/host/test_calib_store.py.
 * Actual production scheduler code is included, hardware access is mocked.
 * The real codec and xcore types are used; flash timing and cross-core locking
 * still need validation on TC275 hardware. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mw/calib/calib_store.h"
#include "mw/sf/sf_frame.h"
#include "mw/xcore/xcore.h"

static uint32 nowMs;
static XcoreRecordLive liveRecord;
static XcoreCalibResult mailbox;
static XcoreJog jog;
static XcoreEncoder encoder;
static XcoreImu imu;
static XcoreEvtFrame events[64];
static unsigned eventCount, writeCount, failureLogs;
static boolean writeResults[3];
static uint8 flashBlob[CALIB_REC_BLOB_LEN];

static uint32 STIME_nowMs(void) { return nowMs; }
static void UART_println(const char *message) { (void)message; }
static void calib_logSlot(const char *label) { (void)label; }
static void calib_flashRead(uint8 *blob) { memcpy(blob, flashBlob, sizeof flashBlob); }
static boolean calib_flashErase(void) { memset(flashBlob, 0, sizeof flashBlob); return TRUE; }
static boolean calib_flashSave(const CalibRecord *rec)
{
    boolean ok;
    assert(writeCount < 3);
    ok = writeResults[writeCount++];
    if (ok) CALIBREC_encode(rec, flashBlob);
    return ok;
}

void XCORE_recordGet(XcoreRecordLive *out) { *out = liveRecord; }
void XCORE_recordSet(const CalibRecord *rec) { liveRecord.rec = *rec; liveRecord.version++; }
boolean XCORE_calibResultTake(XcoreCalibResult *out)
{
    if (!mailbox.pending) return FALSE;
    *out = mailbox;
    mailbox.pending = FALSE;
    return TRUE;
}
uint32 XCORE_jogGet(XcoreJog *out) { *out = jog; return jog.jogSeq; }
uint32 XCORE_motorGetTarget(sint16 *left, sint16 *right, boolean *estop)
{
    *left = 0; *right = 0; *estop = FALSE;
    return 0;
}
void XCORE_encoderRead(XcoreEncoder *out) { *out = encoder; }
void XCORE_imuRead(XcoreImu *out) { *out = imu; }
boolean XCORE_evtPush(const XcoreEvtFrame *frame)
{
    assert(eventCount < sizeof events / sizeof events[0]);
    events[eventCount++] = *frame;
    return TRUE;
}
void XCORE_logln(const char *message)
{
    if (strcmp(message, "CALSAVE failed (flash)") == 0) failureLogs++;
}

#include "calib_store_host.inc"

static unsigned resultCount(void)
{
    unsigned i, n = 0;
    for (i = 0; i < eventCount; i++) if (events[i].cid == SF_CID_DPT_RESULT) n++;
    return n;
}

static const XcoreEvtFrame *result(unsigned index)
{
    unsigned i;
    for (i = 0; i < eventCount; i++)
    {
        if (events[i].cid == SF_CID_DPT_RESULT && index-- == 0) return &events[i];
    }
    assert(0);
    return NULL;
}

static void expectResult(unsigned index, uint8 status, uint8 saved)
{
    const XcoreEvtFrame *frame = result(index);
    assert(frame->type == SF_TYPE_EVT);
    assert(frame->len == CALIB_EVT_RESULT_LEN);
    assert(frame->payload[1] == status);
    assert(frame->payload[CALIB_EVT_RESULT_SAVED] == saved);
}

static void reset(void)
{
    nowMs += 1000;
    memset(&mailbox, 0, sizeof mailbox);
    memset(&jog, 0, sizeof jog);
    memset(&encoder, 0, sizeof encoder);
    memset(&imu, 0, sizeof imu);
    imu.alive = 1u;
    memset(flashBlob, 0, sizeof flashBlob);
    eventCount = writeCount = failureLogs = 0;
    writeResults[0] = writeResults[1] = writeResults[2] = TRUE;
    CALIB_init();
}

static void publish(uint8 status)
{
    unsigned i;
    mailbox.pending = TRUE;
    mailbox.status = status;
    for (i = 0; i < 4; i++)
    {
        mailbox.invert[i] = (i & 1) ? -1 : 1;
        mailbox.delta[i] = (sint32)(20 + i);
    }
}

int main(void)
{
    unsigned i;
    uint8 body[CALIB_REC_SET_LEN] = {0, 2, 3, 1, 1, 1, 1, 1};
    CalibRecord decoded;

    /* Empty mailbox ticks must not fail a held result while wheels coast. */
    reset();
    encoder.pctLeft = 10;
    publish(CALIB_STATUS_DONE);
    CALIB_tick();
    for (i = 0; i < 10; i++) { nowMs += 10; CALIB_tick(); }
    assert(resultCount() == 0 && writeCount == 0);
    encoder.pctLeft = 0;
    nowMs += 490; CALIB_tick();
    assert(resultCount() == 0);
    nowMs += 10; CALIB_tick();
    assert(resultCount() == 1 && writeCount == 1);
    expectResult(0, CALIB_STATUS_DONE, CALIB_SAVED_WRITTEN);
    assert(CALIBREC_decode(flashBlob, &decoded));
    assert(decoded.invert[1] == -1);
    assert(CALIBREC_getI32(&result(0)->payload[6]) == 20);

    /* A failed first attempt followed by success sends exactly one success. */
    reset(); writeResults[0] = FALSE;
    publish(CALIB_STATUS_DONE); CALIB_tick();
    assert(writeCount == 1 && resultCount() == 0);
    for (i = 0; i < 49; i++) { nowMs += 10; CALIB_tick(); }
    assert(writeCount == 1 && resultCount() == 0);
    nowMs += 10; CALIB_tick();
    assert(writeCount == 2 && resultCount() == 1 && failureLogs == 0);
    expectResult(0, CALIB_STATUS_DONE, CALIB_SAVED_WRITTEN);
    for (i = 0; i < 20; i++) { nowMs += 10; CALIB_tick(); }
    assert(resultCount() == 1 && writeCount == 2);

    /* All three attempts must fail before reporting a final failure. */
    reset(); writeResults[0] = writeResults[1] = writeResults[2] = FALSE;
    publish(CALIB_STATUS_DONE); CALIB_tick();
    assert(resultCount() == 0);
    nowMs += 500; CALIB_tick(); assert(resultCount() == 0);
    nowMs += 500; CALIB_tick();
    assert(resultCount() == 1 && writeCount == 3 && failureLogs == 1);
    expectResult(0, CALIB_STATUS_DONE, CALIB_SAVED_FAILED);
    nowMs += 500; CALIB_tick(); assert(writeCount == 3 && resultCount() == 1);

    /* A BUSY response is not a new DONE run and cannot cancel its save. */
    reset(); encoder.pctRight = 10;
    publish(CALIB_STATUS_DONE); CALIB_tick();
    publish(CALIB_STATUS_BUSY); nowMs += 10; CALIB_tick();
    assert(resultCount() == 1 && writeCount == 0);
    expectResult(0, CALIB_STATUS_BUSY, CALIB_SAVED_NONE);
    encoder.pctRight = 0; nowMs += 500; CALIB_tick();
    assert(resultCount() == 2);
    expectResult(1, CALIB_STATUS_DONE, CALIB_SAVED_WRITTEN);

    /* A different queued write/clear cannot confirm the older calibration. */
    reset(); encoder.pctLeft = 10;
    publish(CALIB_STATUS_DONE); CALIB_tick();
    CALIBREC_putI16(&body[8], 2000); CALIBREC_putI16(&body[10], 48);
    CALIB_recordSet(body, sizeof body);
    assert(resultCount() == 1);
    expectResult(0, CALIB_STATUS_DONE, CALIB_SAVED_NONE);
    encoder.pctLeft = 0; nowMs += 500; CALIB_tick();
    assert(writeCount == 1 && resultCount() == 1);
    reset(); encoder.pctLeft = 10;
    publish(CALIB_STATUS_DONE); CALIB_tick(); CALIB_recordClear();
    expectResult(0, CALIB_STATUS_DONE, CALIB_SAVED_NONE);
    encoder.pctLeft = 0; nowMs += 500; CALIB_tick(); assert(writeCount == 0);

    /* Invalid live parameters must not latch an unrelated queued write. */
    reset(); CALIB_recordSet(body, sizeof body);
    liveRecord.rec.fullScaleMmS = 0;
    publish(CALIB_STATUS_DONE); CALIB_tick();
    assert(resultCount() == 1);
    expectResult(0, CALIB_STATUS_DONE, CALIB_SAVED_FAILED);
    nowMs += 500; CALIB_tick(); assert(resultCount() == 1);

    /* IMU command: parked + live IMU, immediate echo then verified save. */
    {
        uint8 imuBody[5] = {1u, 2u, 3u, 150u, 0u};
        reset();
        CALIB_imuSet(imuBody, sizeof imuBody);
        assert(eventCount == 1 && events[0].cid == SF_CID_DPT_REC);
        assert(events[0].payload[20] == 0u);
        assert(liveRecord.rec.imuAxis[0] == 1 && liveRecord.rec.trackMm == 150u);
        nowMs += 500; CALIB_tick();
        assert(writeCount == 1 && eventCount == 2);
        assert(events[1].payload[20] == 1u);
        assert(CALIBREC_decode(flashBlob, &decoded));
        assert(decoded.imuAxis[0] == 1 && decoded.trackMm == 150u);
        reset();
        imu.alive = 0u;
        CALIB_imuSet(imuBody, sizeof imuBody);
        assert(eventCount == 1 && events[0].payload[20] == 3u);
        assert(liveRecord.rec.imuAxis[0] == 0);
        reset();
        encoder.pctLeft = 10;
        CALIB_imuSet(imuBody, sizeof imuBody);
        assert(eventCount == 1 && events[0].payload[20] == 3u);
    }

    puts("PASS: calibration save holds, quiet gate, retry success, final failure, BUSY, replacement operations, invalid parameters");
    return 0;
}
