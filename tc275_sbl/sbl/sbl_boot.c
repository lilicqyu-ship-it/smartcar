/*
 * sbl_boot.c - SBL boot decision + slot jump (doc 24 §5.1)
 *
 * Boot signal (P00.5 on the TC275 lite kit, active low):
 *   one  200 ms flash  -> booting slot A
 *   two  200 ms flashes -> booting slot B
 *   fast blink forever -> safe mode: no bootable slot, SBL parks here with
 *                         the LED hammering so a bench sees it across the
 *                         room (doc 24: "点错误 LED，等 UART 重刷" - the UART
 *                         reflash channel itself is App-side, not in the SBL)
 */
#include "sbl_boot.h"

#include <string.h>

#include "Bsp.h"
#include "IfxPort.h"
#include "../bsp/flash_ota.h"
#include "../mw/ota/ota_boot.h"
#include "../mw/ota/ota_layout.h"
#include "../mw/ota/ota_meta.h"

/* P00.5, the lite kit LED (same pin sbl_led.c drives), active low. */
#define SBL_LED_PORT       &MODULE_P00
#define SBL_LED_PIN        5

/* First boot on a blank DFlash: boot slot A iff its entry looks programmed.
 * Factory bring-up needs this (the very first slot-A flash precedes any
 * metadata); once the first OTA completes, the metadata rules alone. */
#define SBL_ALLOW_FIRST_BOOT   1

typedef void (*SblEntry)(void);

static void sbl_flash(uint8 count, uint32 ms)
{
    uint8 i;

    for (i = 0; i < count; i++)
    {
        IfxPort_setPinLow(SBL_LED_PORT, SBL_LED_PIN);              /* on  */
        waitTime(IfxStm_getTicksFromMilliseconds(BSP_DEFAULT_TIMER, ms));
        IfxPort_setPinHigh(SBL_LED_PORT, SBL_LED_PIN);             /* off */
        waitTime(IfxStm_getTicksFromMilliseconds(BSP_DEFAULT_TIMER, 120u));
    }
}

static void sbl_safeMode(void)
{
    for (;;)
    {
        sbl_flash(1u, 80u);    /* fast hammering: visible across the bench */
    }
}

void SBL_jumpToSlot(uint8 slot)
{
    SblEntry entry = (SblEntry)OTA_slotEntry(slot);

    __disable();
    __dsync();

    /* Plain indirect call: the App's CStart reprograms SP/PSW/PCXI from its
     * own linker symbols within its first instructions, so nothing about
     * this call frame survives or matters. */
    entry();

    sbl_safeMode();   /* an App that returns is not an App */
}

void SBL_boot(void)
{
    OtaMeta       meta;
    OtaBootAction action;
    uint8         firstBootAOk = 0u;
    uint8         commit;

    FLASHOTA_metaAttach();
    memset(&meta, 0, sizeof(meta));

    if (OTAMETA_load() == 0u)
    {
        /* No valid metadata page: factory bring-up path only. */
#if SBL_ALLOW_FIRST_BOOT
        firstBootAOk = FLASHOTA_slotEntrySane(OTA_SLOT_A);
#endif
        OTAMETA_setDefault(&meta, OTA_SLOT_A);
    }
    else
    {
        meta = *OTAMETA_get();
    }

    commit = OTABOOT_decide(&meta, firstBootAOk, &action);
    if ((commit != 0u) && (OTAMETA_commit(&meta) == 0u))
    {
        /* Metadata would not stick (DFlash fault). Rolling back or arming a
         * new image without a durable record would boot a slot the next
         * reset knows nothing about, so park instead (§5.1 conservative). */
        sbl_safeMode();
    }

    switch (action)
    {
        case OTABOOT_JUMP_A:
            sbl_flash(1u, 200u);
            SBL_jumpToSlot(OTA_SLOT_A);
            break;                      /* not reached */

        case OTABOOT_JUMP_B:
            sbl_flash(2u, 200u);
            SBL_jumpToSlot(OTA_SLOT_B);
            break;                      /* not reached */

        case OTABOOT_SAFE_MODE:
        default:
            sbl_safeMode();
            break;                      /* not reached */
    }
}
