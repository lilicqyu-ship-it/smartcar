/*
 * ota_app.c - OTA receiver glue on the App side (doc 24 SS8)
 *
 * All OTA state lives on CPU2 next to the link pump that feeds it; the flash
 * backend runs in this core's context, where the CPU2 watchdog is disabled
 * by design (Cpu2_Main.c), so multi-hundred-ms sector erases inside
 * OTARX_frame() cannot reset the vehicle - they stall the link pump, which
 * costs telemetry and ACK latency for the duration of an erase and nothing
 * else (22 SS5.4 liveness reacts on the C6 side, the pusher is not written
 * yet and will need to tolerate the BEGIN erase gap).
 */
#include "com/ota_app.h"

#include <string.h>

#include "IfxCpu.h"
#include "com/link.h"
#include "bsp/flash_ota.h"
#include "bsp/stime.h"
#include "mw/ota/ota_boot.h"
#include "mw/ota/ota_keys.h"
#include "mw/ota/ota_layout.h"
#include "mw/ota/ota_meta.h"
#include "mw/ota/ota_rx.h"
#include "mw/xcore/xcore.h"

/* CStart0's reset entry (the address the SBL jumped to): comparing it
 * against the slot bases identifies the running image without a build flag. */
extern void _START(void);

/* ---- metadata helpers ------------------------------------------------------ */

/* Load the store, seeding a first-boot default (this slot VALID, other
 * EMPTY) when DFlash has never been written. Returns 1 with *m filled. */
static uint8 otaapp_loadOrSeed(OtaMeta *m)
{
    if (OTAMETA_load() != 0u)
    {
        *m = *OTAMETA_get();
        return 1u;
    }
    OTAMETA_setDefault(m, OTAAPP_activeSlot());
    return 0u;
}

static void otaapp_metaDownloaded(uint8 slot)
{
    OtaMeta m;

    (void)otaapp_loadOrSeed(&m);
    if ((OTABOOT_markDownloaded(&m, slot) != 0u) && (OTAMETA_commit(&m) != 0u))
    {
        XCORE_logln("OTA: slot marked DOWNLOADED");
    }
    else
    {
        XCORE_logln("OTA: meta commit FAILED (downloaded)");
    }
}

static void otaapp_metaAborted(uint8 slot)
{
    OtaMeta m;

    if (OTAMETA_load() == 0u)
    {
        return;      /* nothing recorded -> nothing to retract */
    }
    m = *OTAMETA_get();
    if ((OTABOOT_markAborted(&m, slot) != 0u) && (OTAMETA_commit(&m) != 0u))
    {
        XCORE_logln("OTA: slot abandoned");
    }
}

/* ---- receiver ops ----------------------------------------------------------- */

/* uint8_t-typed trampolines over the boolean-returning flash backend, so the
 * function pointers match OtaRxOps exactly (TASKING is strict about the
 * return type in pointer assignments). */
static uint8_t otaapp_eraseSlot(uint8_t slot)
{
    return (FLASHOTA_eraseSlot((uint8)slot) != FALSE) ? 1u : 0u;
}

static uint8_t otaapp_writeApp(uint8_t slot, uint32_t off,
                               const uint8_t *data, uint32_t len)
{
    return (FLASHOTA_writeApp((uint8)slot, (uint32)off,
                              (const uint8 *)data, (uint32)len) != FALSE) ? 1u : 0u;
}

static uint8_t otaapp_flushApp(uint8_t slot, uint32_t written)
{
    return (FLASHOTA_flushApp((uint8)slot, (uint32)written) != FALSE) ? 1u : 0u;
}

static uint8_t otaapp_readApp(uint8_t slot, uint32_t off, uint32_t len, uint8_t *out)
{
    return (FLASHOTA_readApp((uint8)slot, (uint32)off, (uint32)len,
                             (uint8 *)out) != FALSE) ? 1u : 0u;
}

static void otaapp_send(uint8 sfType, uint8 sfCid, const uint8 *payload, uint8 len)
{
    (void)LINK_send(sfType, sfCid, payload, len);
}

static void otaapp_systemReset(void)
{
    /* OTA_SWAP: back into the SBL, which activates the pending slot
     * (doc 24 SS5.1). Never returns. */
    IfxCpu_triggerSwReset();
}

static OtaRxOps s_ops;

/* ---- self-test confirmation (doc 24 SS5.2) ----------------------------------- */

static uint32 s_bootMs;
static uint8   s_confirmed;
static uint8   s_linkSeenUp;

void OTAAPP_init(void)
{
    FLASHOTA_metaAttach();

    s_ops.activeSlot     = OTAAPP_activeSlot();
    s_ops.pubkey         = OTA_KEYS_DEV;
    s_ops.eraseSlot      = otaapp_eraseSlot;
    s_ops.writeApp       = otaapp_writeApp;
    s_ops.flushApp       = otaapp_flushApp;
    s_ops.readApp        = otaapp_readApp;
    s_ops.metaDownloaded = otaapp_metaDownloaded;
    s_ops.metaAborted    = otaapp_metaAborted;
    s_ops.systemReset    = otaapp_systemReset;
    s_ops.send           = otaapp_send;
    OTARX_init(&s_ops);

    s_bootMs     = STIME_nowMs();
    s_confirmed  = FALSE;
    s_linkSeenUp = FALSE;
}

void OTAAPP_tick(void)
{
    OtaMeta m;

    if (s_confirmed != FALSE)
    {
        return;
    }
    if (LINK_isUp() != FALSE)
    {
        s_linkSeenUp = TRUE;
    }
    if ((s_linkSeenUp == FALSE) ||
        ((uint32)(STIME_nowMs() - s_bootMs) < OTAAPP_CONFIRM_DELAY_MS))
    {
        return;
    }

    /* Up long enough with a live link: this image passed its self-test. */
    s_confirmed = TRUE;
    if (otaapp_loadOrSeed(&m) == 0u)
    {
        XCORE_logln("OTA: blank meta, confirm seeds defaults");
        if (OTAMETA_commit(&m) != FALSE)
        {
            return;
        }
    }
    if (OTABOOT_confirmSelftest(&m) != 0u)
    {
        if (OTAMETA_commit(&m) != FALSE)
        {
            XCORE_logln("OTA: self-test confirmed, slot VALID");
        }
        else
        {
            XCORE_logln("OTA: confirm commit FAILED");
        }
    }
}

uint8 OTAAPP_activeSlot(void)
{
    return ((uint32)&_START >= OTA_SLOT_B_BASE_C) ? OTA_SLOT_B : OTA_SLOT_A;
}

boolean OTAAPP_confirmed(void)
{
    return s_confirmed;
}
