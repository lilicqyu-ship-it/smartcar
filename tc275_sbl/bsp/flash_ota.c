/*
 * flash_ota.c - IfxFlash-backed implementation of bsp/flash_ota.h
 *
 * The command sequences mirror the proven DFlash pattern of tc275_car's
 * mw/calib/calib_store.c (clearStatus -> enterPageMode -> loadPage ->
 * writePage -> waitUnbusy, eraseSector -> waitUnbusy), extended for the
 * 32-byte PFlash page (4 x loadPage2X32) and the per-bank busy bits.
 */
#include "flash_ota.h"

#include <string.h>

#include "IfxFlash.h"
#include "IfxStm.h"    /* STM0 tick for the erase/program timeout window  */
#include "IfxFlash_cfg.h"
#include "../mw/ota/ota_layout.h"

/* Erase/program worst case is a couple of hundred ms per PFlash sector
 * (TC27x datasheet); the budget exists so a stuck FMU surfaces as a FALSE
 * return instead of a permanently busy loop. */
/* 5 s: worst-case sector erase with margin; computed once at first use
 * against the STM0 tick rate the BSP configured. */
static sint32 flashota_timeoutTicks(void)
{
    static sint32 cached = -1;

    if (cached < 0)
    {
        cached = IfxStm_getTicksFromMicroseconds(&MODULE_STM0, 5000000u);
    }
    return cached;
}

/* pFlashTableLog indices of the erasable sectors per slot: PF0 S2..S26 for
 * slot A, PF1 S2..S26 (= table rows 29..53) for slot B (ota_layout.h). */
#define FLASHOTA_PF0_FIRST_SECT  2u
#define FLASHOTA_PF1_FIRST_SECT  (27u + 2u)
#define FLASHOTA_SLOT_SECTORS    25u

/* ---- staged PFlash page ------------------------------------------------------ */

typedef struct
{
    uint8_t  used;              /* staging in progress (0 = idle)            */
    uint32_t pageOff;           /* absolute slot offset of the staged page   */
    uint32_t staged;            /* bytes currently in the buffer (1..32)     */
    uint8_t  page[OTA_PF_PAGE];
    uint8_t  slot;
} FlashOtaStaging;

static FlashOtaStaging s_stage;

/* ---- low-level ---------------------------------------------------------------- */

/* Free-running STM0 lower counter: monotonic without any init, which is all
 * the timeout logic needs. */
static boolean flashota_timedOut(uint32_t start)
{
    uint32_t now = IfxStm_getLower(&MODULE_STM0);

    return (boolean)((sint32)(now - start) > flashota_timeoutTicks());
}

/* Issue-phase critical section: the command register writes must not be
 * interleaved with another flash command from an interrupt context. The
 * FMU itself keeps executing the command once issued, so the wait runs with
 * interrupts enabled. */
static boolean flashota_waitBank(uint32_t busyBits)
{
    uint32_t start = IfxStm_getLower(&MODULE_STM0);

    while ((FLASH0_FSR.U & busyBits) != 0u)
    {
        if (flashota_timedOut(start))
        {
            return FALSE;
        }
    }
    if ((FLASH0_FSR.B.OPER != 0u) || (FLASH0_FSR.B.PROER != 0u))
    {
        IfxFlash_clearStatus(0u);
        return FALSE;
    }
    return TRUE;
}

static boolean flashota_eraseSectorNc(uint32 sectorAddr, uint32_t busyBits)
{
    boolean ok;

    __disable();
    IfxFlash_clearStatus(0u);
    IfxFlash_eraseSector(sectorAddr);
    __enable();

    ok = flashota_waitBank(busyBits);
    IfxFlash_clearStatus(0u);
    return ok;
}

/* Big-endian packing of four bytes into one 32-bit program word: TriCore
 * stores the MSB at the lowest address, so byte i of the image must land in
 * bits [31-8i .. 24-8i] of the word handed to the page buffer. */
/* Page load buffer convention (bench-proven on this silicon via the calib
 * record path, tc275_car 1.0.9/1.0.10): the FMU maps the 32-bit word's LSB
 * to the page's LOWEST byte address, so the word must be packed
 * little-endian. A big-endian pack writes every 4-byte group reversed -
 * a 'TCOM' magic would land as 'M OCT' and no read-back verify can pass. */
static uint32_t flashota_packWord(const uint8_t *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/* Program one whole page at pageAddr (non-cached, page-aligned). */
static boolean flashota_programPageNc(uint32 pageAddr, const uint8_t bytes[OTA_PF_PAGE],
                                      uint32_t busyBits)
{
    boolean ok;
    uint32_t w;

    __disable();
    IfxFlash_clearStatus(0u);
    if (IfxFlash_enterPageMode(pageAddr) != 0u)
    {
        __enable();
        return FALSE;
    }
    for (w = 0u; w < (OTA_PF_PAGE / 4u); w++)
    {
        IfxFlash_loadPage2X32(pageAddr,
                              flashota_packWord(&bytes[(w * 4u) + 0u]),
                              flashota_packWord(&bytes[(w * 4u) + 2u]));
    }
    IfxFlash_writePage(pageAddr);
    __enable();

    ok = flashota_waitBank(busyBits);
    IfxFlash_clearStatus(0u);
    if (ok != FALSE)
    {
        uint32_t i;

        for (i = 0u; i < OTA_PF_PAGE; i++)
        {
            if (*(volatile uint8_t *)(pageAddr + i) != bytes[i])
            {
                return FALSE;
            }
        }
    }
    return ok;
}

static uint32_t flashota_slotBusyBits(uint8 slot)
{
    /* P0BUSY (bit 3) for bank 0 / slot A, P1BUSY (bit 4) for bank 1 / slot B */
    return (slot == OTA_SLOT_B) ? 0x10u : 0x08u;
}

/* ---- staging -------------------------------------------------------------------- */

static boolean flashota_stageFlush(FlashOtaStaging *st)
{
    boolean ok;

    if ((st->used == 0u) || (st->staged == 0u))
    {
        return TRUE;
    }
    while (st->staged < OTA_PF_PAGE)
    {
        st->page[st->staged] = 0xFFu;    /* erased-state padding */
        st->staged++;
    }
    ok = flashota_programPageNc(OTA_slotBaseNc(st->slot) + st->pageOff,
                                st->page, flashota_slotBusyBits(st->slot));
    st->staged = 0u;
    st->pageOff += OTA_PF_PAGE;
    return ok;
}

/* ---- slot ops -------------------------------------------------------------------- */

boolean FLASHOTA_eraseSlot(uint8 slot)
{
    uint32 first = (slot == OTA_SLOT_B) ? FLASHOTA_PF1_FIRST_SECT
                                        : FLASHOTA_PF0_FIRST_SECT;
    uint32_t busyBits = flashota_slotBusyBits(slot);
    uint32 i;

    s_stage.used = 0u;
    for (i = 0u; i < FLASHOTA_SLOT_SECTORS; i++)
    {
        uint32 addr = IfxFlash_pFlashTableLog[first + i].start;

        if (flashota_eraseSectorNc(addr, busyBits) == FALSE)
        {
            return FALSE;
        }
    }
    return TRUE;
}

boolean FLASHOTA_writeApp(uint8 slot, uint32 off,
                          const uint8 *data, uint32 len)
{
    FlashOtaStaging *st = &s_stage;

    if ((slot > OTA_SLOT_B) || (len > OTA_SLOT_SIZE) ||
        (off > (OTA_SLOT_SIZE - len)))
    {
        return FALSE;
    }

    if (st->used == 0u)
    {
        st->used    = 1u;
        st->slot    = slot;
        st->pageOff = off & ~((uint32_t)OTA_PF_PAGE - 1u);
        st->staged  = 0u;
    }

    /* strictly sequential stream: off must be exactly the next byte */
    if ((st->slot != slot) || (off != (st->pageOff + st->staged)))
    {
        return FALSE;
    }
    /* leading gap inside the first page (image shorter than the staging
     * began with) cannot happen on a sequential stream starting at 0, but a
     * misaligned start is refused rather than silently zero-filled */
    if (st->staged == 0u)
    {
        st->pageOff = off & ~((uint32_t)OTA_PF_PAGE - 1u);
        if (off != st->pageOff)
        {
            return FALSE;
        }
    }

    while (len > 0u)
    {
        if (st->staged == OTA_PF_PAGE)
        {
            if (flashota_stageFlush(st) == FALSE)
            {
                return FALSE;
            }
        }
        st->page[st->staged] = *data;
        st->staged++;
        data++;
        len--;
        off++;
    }
    return TRUE;
}

boolean FLASHOTA_flushApp(uint8 slot, uint32 written)
{
    FlashOtaStaging *st = &s_stage;

    if ((st->used == 0u) || (st->slot != slot))
    {
        return FALSE;
    }
    if (st->pageOff + st->staged != written)
    {
        return FALSE;      /* caller's idea of the stream disagrees with ours */
    }
    if (flashota_stageFlush(st) == FALSE)
    {
        return FALSE;
    }
    st->used = 0u;
    return TRUE;
}

boolean FLASHOTA_readApp(uint8 slot, uint32 off, uint32 len, uint8 *out)
{
    uint32_t i;
    uint32_t base = OTA_slotBaseNc(slot);

    for (i = 0u; i < len; i++)
    {
        out[i] = *(volatile uint8_t *)(base + off + i);
    }
    return TRUE;
}

uint8 FLASHOTA_slotEntrySane(uint8 slot)
{
    uint32_t entry = OTA_slotBaseNc(slot) + OTA_SLOT_ENTRY_OFF;
    uint32_t words[2];
    uint32_t k;

    for (k = 0u; k < 2u; k++)
    {
        words[k] = *(volatile uint32_t *)(entry + (k * 4u));
    }
    if ((words[0] == 0xFFFFFFFFu) && (words[1] == 0xFFFFFFFFu))
    {
        return 0u;                 /* erased - never programmed */
    }
    if ((words[0] == 0x00000000u) && (words[1] == 0x00000000u))
    {
        return 0u;                 /* blank - programming artifact */
    }
    return 1u;
}

/* ---- OtaMeta backend -------------------------------------------------------------- */

boolean FLASHOTA_metaErase(uint8 pageIdx)
{
    boolean ok;

    __disable();
    IfxFlash_clearStatus(0u);
    IfxFlash_eraseSector(OTA_metaPageAddr(pageIdx));
    __enable();

    ok = flashota_waitBank(0x02u /* D0BUSY */);
    IfxFlash_clearStatus(0u);
    return ok;
}

boolean FLASHOTA_metaWrite(uint8 pageIdx, const uint8 *data, uint32 len)
{
    uint32_t base = OTA_metaPageAddr(pageIdx);
    uint32_t off  = 0u;

    if ((len == 0u) || (len > OTA_META_PAGE_SIZE))
    {
        return FALSE;
    }
    while (off < len)
    {
        uint8_t page[OTA_DF_PAGE];
        uint32_t i;
        boolean ok;

        for (i = 0u; i < OTA_DF_PAGE; i++)
        {
            page[i] = ((off + i) < len) ? data[off + i] : 0x00u;   /* DFlash0 erased level */
        }
        __disable();
        IfxFlash_clearStatus(0u);
        if (IfxFlash_enterPageMode(base + off) != 0u)
        {
            __enable();
            return FALSE;
        }
        IfxFlash_loadPage2X32(base + off,
                              flashota_packWord(&page[0]),
                              flashota_packWord(&page[4]));
        IfxFlash_writePage(base + off);
        __enable();

        ok = flashota_waitBank(0x02u /* D0BUSY */);
        IfxFlash_clearStatus(0u);
        if (ok == FALSE)
        {
            return FALSE;
        }
        off += OTA_DF_PAGE;
    }
    return TRUE;
}

void FLASHOTA_metaRead(uint8 pageIdx, uint8 *data, uint32 len)
{
    uint32_t base = OTA_metaPageAddr(pageIdx);
    uint32_t i;

    for (i = 0u; i < len; i++)
    {
        data[i] = *(volatile uint8_t *)(base + i);
    }
}

/* ---- backend wiring ----------------------------------------------------------------- */

/* uint8_t-typed trampolines over the boolean returns the OtaMetaBackend
 * contract wants */
static uint8_t metaBk_erase(uint8_t pageIdx)
{
    return (uint8_t)((FLASHOTA_metaErase(pageIdx) != FALSE) ? 1u : 0u);
}

static uint8_t metaBk_write(uint8_t pageIdx, const uint8_t *data, uint32_t len)
{
    return (uint8_t)((FLASHOTA_metaWrite(pageIdx, data, len) != FALSE) ? 1u : 0u);
}

static void metaBk_read(uint8_t pageIdx, uint8_t *data, uint32_t len)
{
    FLASHOTA_metaRead(pageIdx, data, len);
}

static const OtaMetaBackend s_metaBackend =
{
    metaBk_erase,
    metaBk_write,
    metaBk_read
};

void FLASHOTA_metaAttach(void)
{
    OTAMETA_attach(&s_metaBackend);
}
