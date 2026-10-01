/*
 * ota_layout.h - flash partition truth for the TC275 OTA scheme (doc 24 §3/§4)
 *
 * Single source of truth for the SBL/App/linker partition addresses. Pure C99,
 * no target headers: compiled into the SBL (TASKING), the App and the host
 * unit tests unchanged.
 *
 * TC275 (TC27xTP_D-Step) memory facts this rests on, all verified against the
 * derivative data actually used by this workspace:
 *   - PFlash0 2 MB, cached 0x80000000 / non-cached 0xA0000000
 *   - PFlash1 2 MB, cached 0x80200000 / non-cached 0xA0200000
 *     (doc 24 §3 originally wrote 0x8030_0000 for PF1 - wrong; the project
 *     linker file and the TC27x memory map put PF1 at 0x80200000)
 *   - PFlash logical sectors per bank (iLLD IfxFlash_cfg.c pFlashTableLog):
 *     S0..S7 16K, S8..S15 32K, S16..S19 64K, S20..S22 128K, S23..S26 256K
 *   - DFlash0 on TC275 = 128 KB = 16 logical sectors x 8 KB at 0xAF000000
 *   - PFlash program page 32 B, DFlash program page 8 B, erase unit sector
 *
 * Partition (doc 24 §3):
 *   SBL      [0x80000000, 0x80008000)  PF0 S0+S1, 32 KB, never OTA-touched
 *   Slot A   [0x80008000, 0x80200000)  PF0 S2..S26, factory image
 *   Slot B   [0x80208000, 0x80400000)  PF1 S2..S26, OTA target (mirror of A)
 *
 * Slot entry convention: the App image built for a slot places its startup
 * section (".start", the _START symbol of the iLLD CStart) at slot base +
 * 0x20, mirroring the hardware BMHD/STADBM convention (reset code at
 * bank_base + 0x20). The SBL jumps to exactly that address.
 *
 * DFlash0 sector allocation across the vehicle codebase (must stay unique):
 *   sector 15 0xAF01E000  tc275_car calib record (mw/calib/calib_store.c)
 *   sector 14 0xAF01C000  OTA meta page 1 (this file)
 *   sector 13 0xAF01A000  OTA meta page 0 (this file)
 *   sector 12..0          free (doc 24 §4.3 config/black-box reserve)
 */
#ifndef OTA_LAYOUT_H
#define OTA_LAYOUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- program flash slots -------------------------------------------------- */

#define OTA_SLOT_A_BASE_NC      0xA0008000u   /* non-cached alias, flash ops   */
#define OTA_SLOT_A_BASE_C       0x80008000u   /* cached alias, code/entry      */
#define OTA_SLOT_B_BASE_NC      0xA0208000u
#define OTA_SLOT_B_BASE_C       0x80208000u

/* 2 MB bank minus the 32 KB SBL region = PF S2..S26 = 0x1F8000. Both slots
 * are mirror-symmetric: same offset within their bank, same size. */
#define OTA_SLOT_SIZE           0x1F8000u

/* ".start" of the App sits here within the slot (BMHD reset convention). */
#define OTA_SLOT_ENTRY_OFF      0x20u

#define OTA_PF_PAGE             32u           /* PFlash program page          */
#define OTA_DF_PAGE             8u            /* DFlash program page          */

#define OTA_SLOT_A              0u
#define OTA_SLOT_B              1u
#define OTA_SLOT_NONE           0xFFu

/* ---- OTA metadata pages (DFlash0) ------------------------------------------ */

#define OTA_META_PAGE0_ADDR     0xAF01A000u   /* DF0 logical sector 13        */
#define OTA_META_PAGE1_ADDR     0xAF01C000u   /* DF0 logical sector 14        */
#define OTA_META_PAGE_SIZE      0x2000u       /* one DF0 logical sector       */
#define OTA_META_PAGES          2u

#define OTA_META_MAGIC          0x4D4F4354u   /* 'T''C''O''M' as LE bytes     */
#define OTA_META_WIRE_LEN       24u           /* serialized OtaMeta size      */

/* ---- helpers ---------------------------------------------------------------- */

static inline uint32_t OTA_slotBaseNc(uint8_t slot)
{
    return (slot == OTA_SLOT_B) ? OTA_SLOT_B_BASE_NC : OTA_SLOT_A_BASE_NC;
}

static inline uint32_t OTA_slotBaseCached(uint8_t slot)
{
    return (slot == OTA_SLOT_B) ? OTA_SLOT_B_BASE_C : OTA_SLOT_A_BASE_C;
}

static inline uint32_t OTA_slotEntry(uint8_t slot)
{
    return OTA_slotBaseCached(slot) + OTA_SLOT_ENTRY_OFF;
}

static inline uint32_t OTA_metaPageAddr(uint8_t page)
{
    return (page == 1u) ? OTA_META_PAGE1_ADDR : OTA_META_PAGE0_ADDR;
}

#ifdef __cplusplus
}
#endif

#endif /* OTA_LAYOUT_H */
