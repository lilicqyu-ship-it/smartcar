#include "mw/calib/calib_store.h"

#include "IfxFlash.h"
#include "bsp/stime.h"
#include "bsp/uart.h"
#include "bsp/wdg.h"
#include "mw/sf/sf_frame.h"
#include "mw/xcore/xcore.h"

/* ---- storage slot -----------------------------------------------------------
 * Values read out of the iLLD, not from memory (_Impl/IfxFlash_cfg.h and
 * IfxFlash_cfg.c): DF0 base IFXFLASH_DFLASH_START 0xAF000000, logical sector
 * 0x2000, page IFXFLASH_DFLASH_PAGE_LENGTH 8 bytes, and the tables put the
 * HSM log at 0xAF110000.. and the UCB at 0xAF100000.. - clear of the sector
 * used here.
 *
 * Sector 15 = the last one inside the DF0 size SDD SS4.2 states for TC275
 * (128 KB = 16 sectors). iLLD's logical table lists 48 sectors (up to
 * 0xAF05FFFF), which is a family maximum, so the slot sits inside DF0 under
 * both readings, and leaves sectors 0..14 to the §4.3 config/black-box pages
 * when they land - they must not claim sector 15.
 * Before the second DFlash user appears, confirm the real sector count from
 * the device datasheet plus a bench erase/program/read cycle here.
 * Endurance: bench saves <=10/day vs >=1e5 cycles -> decades, no wear
 * levelling (doc 34 SS8.1). */
#define CALIB_SECTOR_ADDR \
    (IFXFLASH_DFLASH_START + (15u * 0x2000u))

/* Wait budgets. Infineon's Flash_Programming_1_KIT_TC275_LK waits with no
 * bound at all; a bound exists here only so a stuck FMU costs a logged write
 * failure, not a hang. The old single 200 ms budget was tight for a DFlash
 * logical-sector erase (the OTA driver allows 5 s for its erases), so a slow
 * but successful erase could be reported as a failure: give erase its own
 * generous budget. The CPU watchdog is serviced inside the wait loop. */
#define CALIB_ERASE_TIMEOUT_MS  2000u
#define CALIB_FLASH_TIMEOUT_MS  200u     /* page mode / 8 B page program */

/* Deferred write only once the bench is quiet this long after the trigger
 * (or the last motion sighting), so a save never overlaps motors running
 * (doc 34 SS8.3). */
#define CALIB_WRITE_IDLE_MS     500u

/* Deferred operation slots: one record write, or CLEAR's erase only. */
#define CALIB_ACT_IDLE   0u
#define CALIB_ACT_WRITE  1u
#define CALIB_ACT_ERASE  2u

typedef struct
{
    uint8       action;          /* CALIB_ACT_*                               */
    CalibRecord rec;
    uint32      quietMs;         /* earliest moment a write may start         */
} PendingWrite;

static PendingWrite g_pending;

/* Arm a deferred operation. CALIB_tick runs it once the bench has been quiet
 * for CALIB_WRITE_IDLE_MS, and re-arms through calib_delayWrite() on motion or
 * a failed flash operation. */
static void calib_queueWrite(uint8 action, const CalibRecord *rec)
{
    g_pending.action  = action;
    g_pending.rec     = *rec;
    g_pending.quietMs = STIME_nowMs();
}

static void calib_delayWrite(void)
{
    g_pending.quietMs = STIME_nowMs() + CALIB_WRITE_IDLE_MS;
}

/* A DONE calibration result holds its EVT 0x22 until the auto-persist has
 * run, so the frame's saved byte is the truth (doc 34 SS9.4). */
static XcoreCalibResult g_holdResult;
static boolean          g_holdActive;

/* ---- low-level flash -------------------------------------------------------- */

/* FSR snapshot of the last failed operation, logged once interrupts are back
 * on (calib_flashSave) so a bench SAVE FAILED says which step broke. */
static uint32 g_flashFailFsr;
static uint8  g_flashFailStep;           /* 1 erase, 2 page mode, 3 program, 4 verify */

/* The erase and write-page command sequences are Safety-ENDINIT protected on
 * TC27x: issued with ENDINIT set, the FMU rejects them (FSR.PROER) and the
 * sector is never touched - which read back as a permanent SAVE FAILED.
 * Infineon's Flash_Programming_1_KIT_TC275_LK example wraps exactly these two
 * calls the same way. Disabling the safety watchdog at boot does NOT clear
 * the ENDINIT bit, so it is needed here regardless. */
static void calib_issueErase(uint32 addr)
{
    uint16 pw = IfxScuWdt_getSafetyWatchdogPassword();

    IfxScuWdt_clearSafetyEndinit(pw);
    IfxFlash_eraseSector(addr);
    IfxScuWdt_setSafetyEndinit(pw);
}

static void calib_issueWritePage(uint32 pageAddr)
{
    uint16 pw = IfxScuWdt_getSafetyWatchdogPassword();

    IfxScuWdt_clearSafetyEndinit(pw);
    IfxFlash_writePage(pageAddr);
    IfxScuWdt_setSafetyEndinit(pw);
}

/* Poll the DF0 busy flag. Runs inside this core's interrupt mask, so the
 * timebase is the free-running STM (no tick interrupt needed) and the CPU
 * watchdog is refreshed here: erase + three page programs can legitimately
 * outlast the ~0.5 s window, and a stuck FMU still hits the per-operation
 * deadline and reports a failed save instead of a reset. A finished command
 * that left PROER / OPER / SQER set also counts as a failure. */
static boolean calib_flashWaitD0(uint8 step)
{
    uint32 deadline = STIME_nowMs() +
                      ((step == 1u) ? CALIB_ERASE_TIMEOUT_MS : CALIB_FLASH_TIMEOUT_MS);

    while (FLASH0_FSR.B.D0BUSY != 0u)
    {
        WDG_serviceCpu();
        if ((sint32)(STIME_nowMs() - deadline) >= 0)
        {
            g_flashFailFsr  = FLASH0_FSR.U;
            g_flashFailStep = step;
            return FALSE;
        }
    }
    if ((FLASH0_FSR.B.PROER != 0u) || (FLASH0_FSR.B.OPER != 0u) ||
        (FLASH0_FSR.B.SQER != 0u))
    {
        g_flashFailFsr  = FLASH0_FSR.U;
        g_flashFailStep = step;
        return FALSE;
    }
    return TRUE;
}

/* Erase + program the blob into the slot, then read the record bytes back.
 * iLLD gives a 20 B record and an 8 B DFlash page (IFXFLASH_DFLASH_PAGE_
 * LENGTH), and ECC is computed per page, so the blob is programmed one page
 * at a time with its own enter/load/write sequence; the last page is zero
 * padded and the padding is not part of the read-back compare. */
#define CALIB_FLASH_PAGE_LEN IFXFLASH_DFLASH_PAGE_LENGTH
#define CALIB_FLASH_PAGE_CNT \
    ((CALIB_REC_BLOB_LEN + CALIB_FLASH_PAGE_LEN - 1u) / CALIB_FLASH_PAGE_LEN)

static boolean calib_flashWritePage(uint32 pageAddr, const uint8 *bytes)
{
    boolean ok;

    IfxFlash_clearStatus(0u);
    ok = (IfxFlash_enterPageMode(pageAddr) == 0u);
    if (ok != FALSE)
    {
        ok = calib_flashWaitD0(2u);    /* page mode must not race the erase */
    }
    if (ok != FALSE)
    {
        IfxFlash_loadPage2X32(pageAddr,
                              (uint32)CALIBREC_getI32(&bytes[0]),
                              (uint32)CALIBREC_getI32(&bytes[4]));
        calib_issueWritePage(pageAddr);
        ok = calib_flashWaitD0(3u);
    }
    IfxFlash_clearStatus(0u);
    return ok;
}

static boolean calib_flashWrite(const uint8 *blob)
{
    uint8  page[CALIB_FLASH_PAGE_LEN];
    uint32 p;
    uint32 i;

    for (p = 0u; p < CALIB_FLASH_PAGE_CNT; p++)
    {
        for (i = 0u; i < CALIB_FLASH_PAGE_LEN; i++)
        {
            uint32 idx = (p * CALIB_FLASH_PAGE_LEN) + i;

            page[i] = (idx < CALIB_REC_BLOB_LEN) ? blob[idx] : 0u;
        }
        if (calib_flashWritePage(CALIB_SECTOR_ADDR +
                                 (p * CALIB_FLASH_PAGE_LEN), page) == FALSE)
        {
            return FALSE;
        }
    }

    for (i = 0u; i < CALIB_REC_BLOB_LEN; i++)
    {
        if (*(volatile uint8 *)(CALIB_SECTOR_ADDR + i) != blob[i])
        {
            g_flashFailFsr  = FLASH0_FSR.U;
            g_flashFailStep = 4u;
            return FALSE;
        }
    }
    return TRUE;
}

static void calib_flashRead(uint8 *blob)
{
    uint32 i;

    for (i = 0u; i < CALIB_REC_BLOB_LEN; i++)
    {
        blob[i] = *(volatile uint8 *)(CALIB_SECTOR_ADDR + i);
    }
}

/* Encode scratch buffer. Kept at file scope rather than as a calib_flashSave
 * local because the robot task runs on 2 x configMINIMAL_STACK_SIZE. */
static uint8 g_opBlob[CALIB_REC_BLOB_LEN];

/* Save sequence per doc 34 SS8.3: feed -> mask -> erase + program + read
 * back -> unmask -> feed. Called from the robot task, never from a critical
 * section, so the plain mask/unmask is safe here. The masked window is tens
 * of ms in practice (bounded by CALIB_FLASH_TIMEOUT_MS per operation); CPU0
 * is the only core that fetches from DFlash, so CPU1/CPU2 do not stall. */
static boolean calib_flashSave(const CalibRecord *rec)
{
    boolean ok;

    CALIBREC_encode(rec, g_opBlob);

    WDG_serviceCpu();
    __disable();
    IfxFlash_clearStatus(0u);
    calib_issueErase(CALIB_SECTOR_ADDR);
    ok = calib_flashWaitD0(1u);
    if (ok != FALSE)
    {
        ok = calib_flashWrite(g_opBlob);
    }
    IfxFlash_clearStatus(0u);
    __enable();
    WDG_serviceCpu();
    if (ok == FALSE)
    {
        sint32 v[2];

        v[0] = (sint32)g_flashFailStep;  /* 1 erase 2 pagemode 3 program 4 verify */
        v[1] = (sint32)g_flashFailFsr;
        XCORE_logi("CALSAVE FAIL step/FSR=", v, 2u);
    }
    return ok;
}

static boolean calib_flashErase(void)
{
    boolean ok;

    WDG_serviceCpu();
    __disable();
    IfxFlash_clearStatus(0u);
    calib_issueErase(CALIB_SECTOR_ADDR);
    ok = calib_flashWaitD0(1u);
    IfxFlash_clearStatus(0u);
    __enable();
    WDG_serviceCpu();
    return ok;
}

/* ---- live-record plumbing --------------------------------------------------- */

/* EVT 0x23 echo of the live record. crcOk rides the source: only the default
 * fallback (no record / failed validation, doc 34 SS8.1) reads 0. */
void CALIB_sendRecord(void)
{
    XcoreEvtFrame   frame;
    XcoreRecordLive live;

    XCORE_recordGet(&live);

    frame.type = SF_TYPE_EVT;
    frame.cid  = SF_CID_DPT_REC;
    frame.len  = CALIB_EVT_REC_LEN;
    CALIBREC_buildEvtRec(frame.payload, &live.rec,
                         (live.rec.src == CALIB_SRC_DEFAULT) ? 0u : 1u);
    (void)XCORE_evtPush(&frame);
}

static void calib_sendResultEvt(const XcoreCalibResult *res, uint8 saved)
{
    XcoreEvtFrame frame;

    frame.type = SF_TYPE_EVT;
    frame.cid  = SF_CID_DPT_RESULT;
    frame.len  = CALIB_EVT_RESULT_LEN;
    CALIBREC_buildEvtResult(frame.payload, res->status, res->invert,
                            res->delta, saved);
    (void)XCORE_evtPush(&frame);
}

/* Merge an invert set into the live record and queue the write (doc 34
 * SS8.3: 0x70 success auto-persists, everything else is kept). */
static void calib_persistInvert(const sint8 invert[CALIB_REC_WHEELS])
{
    XcoreRecordLive live;
    CalibRecord     rec;
    uint8           i;

    XCORE_recordGet(&live);
    rec = live.rec;
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        rec.invert[i] = invert[i];
    }
    if (CALIBREC_paramsOk(&rec) == 0u)
    {
        return;                  /* live params corrupted: refuse to persist  */
    }
    rec.src = CALIB_SRC_DFLASH;
    XCORE_recordSet(&rec);

    calib_queueWrite(CALIB_ACT_WRITE, &rec);
}

/* ---- vehicle-quiet gate ------------------------------------------------------ */

static uint8 calib_motionSeen(void)
{
    XcoreJog        jog;
    XcoreEncoder    enc;
    sint16          tgtL, tgtR;
    boolean         tgtEstop;
    uint8           i;

    (void)XCORE_jogGet(&jog);
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        if (jog.duty[i] != 0)
        {
            return 1u;
        }
    }

    (void)XCORE_motorGetTarget(&tgtL, &tgtR, &tgtEstop);
    if ((tgtL != 0) || (tgtR != 0))
    {
        return 1u;
    }

    XCORE_encoderRead(&enc);
    if ((enc.pctLeft != 0) || (enc.pctRight != 0))
    {
        return 1u;
    }
    return 0u;
}

/* ---- public ----------------------------------------------------------------- */

void CALIB_init(void)
{
    uint8       blob[CALIB_REC_BLOB_LEN];
    CalibRecord rec;

    calib_flashRead(blob);
    if (CALIBREC_decode(blob, &rec) != 0u)
    {
        UART_println("CALIBREC loaded from DFLASH");
    }
    else
    {
        /* CALIBREC_decode already leaves the defaults with src=DEFAULT */
        UART_println("CALIBREC invalid, defaults");
    }
    XCORE_recordSet(&rec);

    g_pending.action = CALIB_ACT_IDLE;
    g_holdActive     = FALSE;
}

static void calib_handleResult(void)
{
    XcoreCalibResult res;

    if (g_holdActive != FALSE)
    {
        /* A second DONE result arrived while the first save is still in
         * flight: answer the older one as if the save failed, then follow
         * the newer (cannot legitimately happen - one run at a time). */
        calib_sendResultEvt(&g_holdResult, CALIB_SAVED_FAILED);
        g_holdActive = FALSE;
    }

    if (XCORE_calibResultTake(&res) == FALSE)
    {
        return;
    }

    if (res.status == CALIB_STATUS_DONE)
    {
        calib_persistInvert(res.invert);
        if (g_pending.action == CALIB_ACT_WRITE)
        {
            g_holdResult = res;
            g_holdActive = TRUE;
            return;              /* the frame rides the save outcome */
        }
        /* persistence refused (invalid live params): say so honestly */
        calib_sendResultEvt(&res, CALIB_SAVED_FAILED);
        return;
    }

    calib_sendResultEvt(&res, CALIB_SAVED_NONE);
}

void CALIB_recordSet(const uint8 *data, uint8 len)
{
    XcoreRecordLive live;
    CalibRecord     rec;

    XCORE_recordGet(&live);
    rec = live.rec;
    if (CALIBREC_recSetDecode(data, len, &rec) == 0u)
    {
        /* Invalid body: live state untouched, the echo says what is in force. */
        CALIB_sendRecord();
        return;
    }

    rec.src = CALIB_SRC_ONLINE;
    XCORE_recordSet(&rec);
    CALIB_sendRecord();

    calib_queueWrite(CALIB_ACT_WRITE, &rec);
}

void CALIB_recordClear(void)
{
    CalibRecord rec;

    CALIBREC_fillDefaults(&rec);
    XCORE_recordSet(&rec);
    CALIB_sendRecord();

    calib_queueWrite(CALIB_ACT_ERASE, &rec);
}

/* ---- jog encoder counts (EVT 0x26) ------------------------------------------ */

#define JOG_CNT_PERIOD_MS   100u   /* 10 Hz while any motor is jogging       */
#define JOG_CNT_TAIL_MS     400u   /* keep reporting while the wheel coasts  */

/* While a 0x71 jog is active, report every wheel's count delta since the jog
 * started, so the bench can see WHICH encoder moves and with WHICH sign when
 * one motor is driven. Payload {on u8, delta i32x4 LE, motor A..D order}; on=0
 * marks the tail frames after the last jog duty went to zero. */
static void calib_jogCountsTick(void)
{
    static boolean wasActive;
    static uint32  lastActiveMs;
    static uint32  nextMs;
    static sint32  base[CALIB_REC_WHEELS];
    XcoreJog       jog;
    XcoreEncoder   enc;
    XcoreEvtFrame  frame;
    boolean        active = FALSE;
    uint32         now = STIME_nowMs();
    uint8          i;

    (void)XCORE_jogGet(&jog);
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        if (jog.duty[i] != 0)
        {
            active = TRUE;
        }
    }

    XCORE_encoderRead(&enc);
    if ((active != FALSE) && (wasActive == FALSE))
    {
        /* new press: counts restart from zero */
        for (i = 0u; i < CALIB_REC_WHEELS; i++)
        {
            base[i] = enc.raw[i];
        }
        nextMs = now;
    }
    if (active != FALSE)
    {
        lastActiveMs = now;
    }
    wasActive = active;

    if ((active == FALSE) && ((sint32)(now - (lastActiveMs + JOG_CNT_TAIL_MS)) >= 0))
    {
        return;                          /* idle, or tail already sent        */
    }
    if ((sint32)(now - nextMs) < 0)
    {
        return;
    }
    nextMs = now + JOG_CNT_PERIOD_MS;

    frame.type       = SF_TYPE_EVT;
    frame.cid        = SF_CID_EVT_JOG_CNT;
    frame.len        = 17u;
    frame.payload[0] = (active != FALSE) ? 1u : 0u;
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        CALIBREC_putI32(&frame.payload[1u + (i * 4u)], enc.raw[i] - base[i]);
    }
    (void)XCORE_evtPush(&frame);
}

void CALIB_tick(void)
{
    calib_jogCountsTick();

    /* Drain CPU1's result mailbox on the 10 ms rhythm: one frame per run. */
    calib_handleResult();

    if (g_pending.action == CALIB_ACT_IDLE)
    {
        return;
    }

    if (calib_motionSeen() != 0u)
    {
        calib_delayWrite();
        return;
    }
    if ((sint32)(STIME_nowMs() - g_pending.quietMs) < 0)
    {
        return;
    }

    if (g_pending.action == CALIB_ACT_ERASE)
    {
        if (calib_flashErase() != FALSE)
        {
            g_pending.action = CALIB_ACT_IDLE;
        }
        else
        {
            calib_delayWrite();
        }
        return;
    }

    {
        uint8 saved;

        saved = (calib_flashSave(&g_pending.rec) != FALSE)
                    ? CALIB_SAVED_WRITTEN : CALIB_SAVED_FAILED;

        if (g_holdActive != FALSE)
        {
            calib_sendResultEvt(&g_holdResult, saved);
            g_holdActive = FALSE;
        }

        if (saved == CALIB_SAVED_WRITTEN)
        {
            g_pending.action = CALIB_ACT_IDLE;
        }
        else
        {
            /* retry quietly; the result frame has already gone out */
            calib_delayWrite();
        }
    }
}
