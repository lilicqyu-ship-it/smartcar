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
#define CALIB_SECTOR_ADDR_128K \
    (IFXFLASH_DFLASH_START + (15u * 0x2000u))   /* DF0 = 128 KB variant  */
#define CALIB_SECTOR_ADDR_64K \
    (IFXFLASH_DFLASH_START + (7u * 0x2000u))    /* DF0 = 64 KB variant   */
#define CALIB_SECTOR_ADDR_DEFAULT CALIB_SECTOR_ADDR_128K

/* DF0's real size is variant-dependent (128 KB on TC275, 64 KB on the T
 * variants per the Infineon product brief; DF1 is a separate 256 KB bank)
 * while the iLLD sector table lists the 384 KB family maximum. The bench
 * proved what happens when the slot sits past the real array: the erase
 * command pulses busy and does nothing, every read of the address returns
 * all zeros, and a save can never succeed. CALIB_init therefore probes the
 * array boundary with BYTE reads - the bench showed byte reads in the DF0
 * module decode are benign (0 out, no trap) where v1.0.6's word reads into
 * the HSM-owned region trapped the core - and selects the LAST 8 KB logical
 * sector inside the real array. */
static uint32 g_calibSectorAddr = CALIB_SECTOR_ADDR_128K;

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

/* A deferred operation gives up after this many failed flash attempts. The
 * v1.0.1 behavior retried forever: every attempt is another erase/program
 * cycle on the single slot and another multi-100-ms __disable() window on
 * CPU0, and an operation that fails deterministically (byte order, wiring,
 * FMU state) never self-heals - it just hammers the slot forever. Bounded
 * now: the live record stays applied in RAM for this power cycle; only the
 * final outcome is sent in the EVT, and the console says why. */
#define CALIB_FLASH_RETRIES  3u

typedef struct
{
    uint8       action;          /* CALIB_ACT_*                               */
    CalibRecord rec;
    uint32      quietMs;         /* earliest moment a write may start         */
    uint8       attempts;        /* failed flash attempts so far              */
} PendingWrite;

static PendingWrite g_pending;
static void calib_finishHeldResult(uint8 saved);

/* Arm a deferred operation. CALIB_tick runs it once the bench has been quiet
 * for CALIB_WRITE_IDLE_MS, and re-arms through calib_delayWrite() on motion or
 * a failed flash operation. */
static void calib_queueWrite(uint8 action, const CalibRecord *rec)
{
    /* A replacement operation cannot confirm the older calibration's save. */
    calib_finishHeldResult(CALIB_SAVED_NONE);
    g_pending.action   = action;
    g_pending.rec      = *rec;
    g_pending.quietMs  = STIME_nowMs();
    g_pending.attempts = 0u;
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
static uint8  g_flashFailStep;           /* 1 erase, 2 page mode, 3 program, 4 verify, 5 endinit */


/* Safety-ENDINIT clear/set with a BOUNDED read-back wait.
 *
 * The bench settled this twice over:
 *  - v1.0.2 (no unlock at all): the erase command is silently dropped - no
 *    FSR error flag, D0BUSY never rises, the sector still reads its old
 *    content. DFlash erase/program commands ARE Safety-ENDINIT protected on
 *    TC27x; Infineon's Flash_Programming_1_KIT_TC275_LK example wraps exactly
 *    these two commands, and the FSR captured at the failure backs it (no
 *    OPER/SQER/PROER: the write never reached the FMU).
 *  - v1.0.1 (iLLD IfxScuWdt_clearSafetyEndinit/setSafetyEndinit): those
 *    inlines END in an unbounded read-back spin ("while (ENDINIT != ...)"),
 *    which inside this file's __disable() window - where the CPU watchdog
 *    cannot be serviced mid-sequence - turns any refusal into a dead core.
 *
 * So: the same CON0 register sequence as iLLD, but the wait is bounded and a
 * refusal fails the save with a logged step instead of hanging the core. The
 * command write follows within a few cycles of the clear, like the official
 * example. */
#define CALIB_ENDINIT_WAIT_LOOPS  20000u

static boolean calib_endinitClear(void)
{
    uint16 pw = IfxScuWdt_getSafetyWatchdogPassword();
    uint32 n;

    if (SCU_WDTS_CON0.B.LCK)
    {
        SCU_WDTS_CON0.U = (1u << IFX_SCU_WDTS_CON0_ENDINIT_OFF) |
                          (0u << IFX_SCU_WDTS_CON0_LCK_OFF) |
                          ((uint32)pw << IFX_SCU_WDTS_CON0_PW_OFF) |
                          ((uint32)SCU_WDTS_CON0.B.REL << IFX_SCU_WDTS_CON0_REL_OFF);
    }
    SCU_WDTS_CON0.U = (0u << IFX_SCU_WDTS_CON0_ENDINIT_OFF) |
                      (1u << IFX_SCU_WDTS_CON0_LCK_OFF) |
                      ((uint32)pw << IFX_SCU_WDTS_CON0_PW_OFF) |
                      ((uint32)SCU_WDTS_CON0.B.REL << IFX_SCU_WDTS_CON0_REL_OFF);

    for (n = 0u; n < CALIB_ENDINIT_WAIT_LOOPS; n++)
    {
        if (SCU_WDTS_CON0.B.ENDINIT == 0u)
        {
            return TRUE;
        }
    }
    g_flashFailFsr  = FLASH0_FSR.U;
    g_flashFailStep = 5u;
    return FALSE;
}

static boolean calib_endinitSet(void)
{
    uint16 pw = IfxScuWdt_getSafetyWatchdogPassword();
    uint32 n;

    if (SCU_WDTS_CON0.B.LCK)
    {
        SCU_WDTS_CON0.U = (1u << IFX_SCU_WDTS_CON0_ENDINIT_OFF) |
                          (0u << IFX_SCU_WDTS_CON0_LCK_OFF) |
                          ((uint32)pw << IFX_SCU_WDTS_CON0_PW_OFF) |
                          ((uint32)SCU_WDTS_CON0.B.REL << IFX_SCU_WDTS_CON0_REL_OFF);
    }
    SCU_WDTS_CON0.U = (1u << IFX_SCU_WDTS_CON0_ENDINIT_OFF) |
                      (1u << IFX_SCU_WDTS_CON0_LCK_OFF) |
                      ((uint32)pw << IFX_SCU_WDTS_CON0_PW_OFF) |
                      ((uint32)SCU_WDTS_CON0.B.REL << IFX_SCU_WDTS_CON0_REL_OFF);

    for (n = 0u; n < CALIB_ENDINIT_WAIT_LOOPS; n++)
    {
        if (SCU_WDTS_CON0.B.ENDINIT != 0u)
        {
            return TRUE;
        }
    }
    g_flashFailFsr  = FLASH0_FSR.U;
    g_flashFailStep = 5u;
    return FALSE;
}

/* DFlash page load buffer maps the 32-bit word's LSB to the page's LOWEST
 * byte address on this silicon - the bench proved it with the v1.0.9
 * read-back: a big-endian pack wrote "1CDS" where the record magic "SDC1"
 * was expected, every 4-byte group reversed. Pack little-endian (LSB
 * first) - which is also what the original CALIBREC_getI32 assembly did. */
static uint32 calib_packWord(const uint8 *b)
{
    return (uint32)b[0] | ((uint32)b[1] << 8) |
           ((uint32)b[2] << 16) | ((uint32)b[3] << 24);
}

/* Poll the DF0 busy flag. Runs with interrupts ENABLED (v1.0.5: only the
 * command-issue phase is masked, the proven flash_ota.c pattern), so the
 * loop feeds the CPU watchdog and FreeRTOS/CPU2 keep running through a long
 * erase. Polls FABUSY|D0BUSY together: which bit the DF0 bank actually
 * signals is exactly what the bench is trying to settle, and waiting on
 * both is safe for either answer. A finished command that left PROER /
 * OPER / SQER set also counts as a failure. */
static boolean calib_flashWaitD0(uint8 step)
{
    uint32 deadline = STIME_nowMs() +
                      ((step == 1u) ? CALIB_ERASE_TIMEOUT_MS : CALIB_FLASH_TIMEOUT_MS);

    while ((FLASH0_FSR.U & 0x03u) != 0u)      /* FABUSY(bit0) | D0BUSY(bit1) */
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

    /* Command-issue phase masked only (flash_ota.c pattern); the wait runs
     * unmasked and feeds the watchdog. */
    __disable();
    IfxFlash_clearStatus(0u);
    if (IfxFlash_enterPageMode(pageAddr) != 0u)
    {
        __enable();
        g_flashFailFsr  = FLASH0_FSR.U;
        g_flashFailStep = 2u;
        return FALSE;
    }
    IfxFlash_loadPage2X32(pageAddr,
                          calib_packWord(&bytes[0]),
                          calib_packWord(&bytes[4]));
    ok = calib_endinitClear();
    if (ok != FALSE)
    {
        IfxFlash_writePage(pageAddr);
        calib_endinitSet();
    }
    __enable();

    if (ok != FALSE)
    {
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

            page[i] = (idx < CALIB_REC_BLOB_LEN) ? blob[idx] : 0x00u;
        }
        if (calib_flashWritePage(g_calibSectorAddr +
                                 (p * CALIB_FLASH_PAGE_LEN), page) == FALSE)
        {
            return FALSE;
        }
    }

    for (i = 0u; i < CALIB_REC_BLOB_LEN; i++)
    {
        if (*(volatile uint8 *)(g_calibSectorAddr + i) != blob[i])
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
        blob[i] = *(volatile uint8 *)(g_calibSectorAddr + i);
    }
}

/* Encode scratch buffer. Kept at file scope rather than as a calib_flashSave
 * local because the robot task runs on 2 x configMINIMAL_STACK_SIZE. */
static uint8 g_opBlob[CALIB_REC_BLOB_LEN];

/* First 20 slot bytes as five u32s (big-endian byte order, matching the page
 * load convention) - the bench view of what an erase actually did to the
 * slot. Decimal on the console; decode by hand. */
static void calib_logSlot(const char *label)
{
    volatile uint8 *slot = (volatile uint8 *)g_calibSectorAddr;
    uint32 v[5];
    uint8  i;

    for (i = 0u; i < 5u; i++)
    {
        v[i] = ((uint32)slot[(i * 4u) + 0u] << 24) |
               ((uint32)slot[(i * 4u) + 1u] << 16) |
               ((uint32)slot[(i * 4u) + 2u] << 8)  |
               (uint32)slot[(i * 4u) + 3u];
    }
    XCORE_logu(label, v, 5u);
}

/* Save sequence, v1.0.5. ONLY the command-issue phase runs masked (the
 * production flash_ota.c pattern); the erase wait and the erase-verify run
 * with interrupts on, feeding the CPU watchdog from the wait loop while
 * FreeRTOS and CPU2 keep running. This undoes the v1.0.4 diagnostic design,
 * whose array-byte poll inside the interrupt mask stalled CPU0 on the bus:
 * flash arrays are not readable while their bank is busy, the watchdog chain
 * then reset into an FMU state a warm reset does not clear (only power does)
 * and the board looped silently - the 2026-10-02 bench hang.
 * The erase-verify is a single read after busy-clear plus a settle delay:
 * v1.0.4's FSR triplet proved the erase command IS accepted (D0BUSY pulses)
 * yet the slot byte still read non-0xFF, so the settle+read answers whether
 * a completed erase ever reaches the array. */
static boolean calib_flashSave(const CalibRecord *rec)
{
    boolean ok;
    uint32  fsr0;
    uint32  fsr1;

    CALIBREC_encode(rec, g_opBlob);

    WDG_serviceCpu();
    calib_logSlot("CALSLOT0=");          /* slot state entering the save     */
    {
        /* Per-attempt verdict data (byte reads are bench-proven benign):
         * the slot the firmware actually uses + the two DF0 boundary words
         * the boot probe decided on - so a missed boot banner still leaves
         * the full picture in every save attempt. */
        sint32 dbg[1];

        dbg[0] = (sint32)g_calibSectorAddr;
        XCORE_logi("CALSEC=", dbg, 1u);
    }
    fsr0 = FLASH0_FSR.U;
    __disable();                          /* issue phase only                 */
    IfxFlash_clearStatus(0u);
    ok = calib_endinitClear();
    if (ok != FALSE)
    {
        IfxFlash_eraseSector(g_calibSectorAddr);
        calib_endinitSet();
    }
    fsr1 = FLASH0_FSR.U;                  /* right after the command sequence */
    __enable();

    if (ok != FALSE)
    {
        ok = calib_flashWaitD0(1u);       /* unmasked, feeds the watchdog     */
    }
    if (ok != FALSE)
    {
        STIME_delayMs(50u);               /* settle past any residual busy    */
        if (*(volatile uint8 *)g_calibSectorAddr != 0x00u)
        {
            g_flashFailFsr  = FLASH0_FSR.U;
            g_flashFailStep = 1u;
            ok = FALSE;
        }
    }
    if (ok != FALSE)
    {
        ok = calib_flashWrite(g_opBlob);
    }
    IfxFlash_clearStatus(0u);
    WDG_serviceCpu();
    if (ok == FALSE)
    {
        sint32 v[4];

        /* step: 1 erase 2 pagemode 3 program 4 verify 5 endinit */
        v[0] = (sint32)g_flashFailStep;
        v[1] = (sint32)fsr0;             /* FSR before anything this attempt */
        v[2] = (sint32)fsr1;             /* FSR right after the command      */
        v[3] = (sint32)g_flashFailFsr;   /* FSR at the failure verdict       */
        XCORE_logi("CALSAVE FAIL st/FSR0/1/2=", v, 4u);
        calib_logSlot("CALSLOT1=");      /* slot state after the failed save */
    }
    return ok;
}

static boolean calib_flashErase(void)
{
    boolean ok;

    WDG_serviceCpu();
    __disable();                          /* issue phase only                 */
    IfxFlash_clearStatus(0u);
    ok = calib_endinitClear();
    if (ok != FALSE)
    {
        IfxFlash_eraseSector(g_calibSectorAddr);
        calib_endinitSet();
    }
    __enable();
    ok = calib_flashWaitD0(1u);           /* unmasked, feeds the watchdog     */
    IfxFlash_clearStatus(0u);
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
    int32_t       delta[CALIB_REC_WHEELS];
    uint8         i;

    /* xcore's mailbox is iLLD-typed (sint32 is `long` on TriCore) and the pure
     * C99 codec takes stdint int32_t (`int`), so the element types differ even
     * though the width does not: copy across instead of casting the pointer. */
    for (i = 0u; i < CALIB_REC_WHEELS; i++)
    {
        delta[i] = res->delta[i];
    }

    frame.type = SF_TYPE_EVT;
    frame.cid  = SF_CID_DPT_RESULT;
    frame.len  = CALIB_EVT_RESULT_LEN;
    CALIBREC_buildEvtResult(frame.payload, res->status, res->invert,
                            delta, saved);
    (void)XCORE_evtPush(&frame);
}

static void calib_finishHeldResult(uint8 saved)
{
    if (g_holdActive != FALSE)
    {
        calib_sendResultEvt(&g_holdResult, saved);
        g_holdActive = FALSE;
    }
}

/* Merge an invert set into the live record and queue the write (doc 34
 * SS8.3: 0x70 success auto-persists, everything else is kept). */
static boolean calib_persistInvert(const sint8 invert[CALIB_REC_WHEELS])
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
        return FALSE;            /* live params corrupted: refuse to persist  */
    }
    rec.src = CALIB_SRC_DFLASH;
    XCORE_recordSet(&rec);

    calib_queueWrite(CALIB_ACT_WRITE, &rec);
    return TRUE;
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
    calib_logSlot("CALSLOT=");            /* bench: raw slot bytes at boot    */
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

    if (XCORE_calibResultTake(&res) == FALSE)
    {
        return;
    }

    if (res.status == CALIB_STATUS_DONE)
    {
        if (calib_persistInvert(res.invert) != FALSE)
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
        else if (++g_pending.attempts >= CALIB_FLASH_RETRIES)
        {
            /* Give up, loudly: the live record already answered with the
             * defaults, but a stale DFlash copy would resurface at the next
             * boot. The console line is the bench operator's cue. */
            g_pending.action = CALIB_ACT_IDLE;
            XCORE_logln("CALCLEAR failed (flash)");
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

        if (saved == CALIB_SAVED_WRITTEN)
        {
            g_pending.action = CALIB_ACT_IDLE;
            calib_finishHeldResult(saved);
        }
        else if (++g_pending.attempts >= CALIB_FLASH_RETRIES)
        {
            /* Give up, loudly: the calibrated record stays applied in RAM
             * (signs, closed-loop gate) for this power cycle only; the
             * result frame now reports the final failure with saved=2. */
            g_pending.action = CALIB_ACT_IDLE;
            calib_finishHeldResult(saved);
            XCORE_logln("CALSAVE failed (flash)");
        }
        else
        {
            /* Keep the result held: a later attempt may still succeed. */
            calib_delayWrite();
        }
    }
}
