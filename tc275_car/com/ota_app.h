/*
 * ota_app.h - OTA receiver glue on the App side (doc 24 SS5.2/SS5.3, SS8)
 *
 * Owns everything OTA on CPU2, the link core, so the OtaMeta/OTARX module
 * state is single-core and needs no locking:
 *   - OtaRxOps wiring: flash backend (bsp/flash_ota), metadata store
 *     (OTAMETA double page in DFlash0 13/14), ACK/STATUS frames out through
 *     LINK_send, SWAP -> software reset into the SBL
 *   - the self-test confirmation of doc 24 SS5.2: once the App has been up
 *     for OTAAPP_CONFIRM_DELAY_MS and the SPI link has come up at least once
 *     (the firmware arrived over it, so a PENDING_VERIFY image without a
 *     link is a failed image), the active slot is marked VALID and the SBL's
 *     boot-attempt counter is retired
 *   - slot self-detection from the _START entry address, so the running
 *     image knows which slot it occupies without build flags
 */
#ifndef OTA_APP_H
#define OTA_APP_H

#include "Ifx_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Uptime before the self-test confirmation is committed (doc 24 SS5.2:
 * three-core sync passed long before, link seen up, watchdog serviced by
 * the running scheduler). */
#define OTAAPP_CONFIRM_DELAY_MS  5000u

/* CPU2, after LINK_init() and STIME_init(). Attaches the flash backend,
 * detects the running slot from the entry address and wires OTARX. */
void OTAAPP_init(void);

/* CPU2 superloop, once per cycle (next to LINK_main). Runs the one-shot
 * self-test confirmation. */
void OTAAPP_tick(void);

/* Slot this image was linked/entered in: OTA_SLOT_A / OTA_SLOT_B, derived
 * from the _START address (slot base + 0x20 per the SBL entry convention). */
uint8 OTAAPP_activeSlot(void);

/* TRUE once the self-test confirmation has been committed (or had nothing
 * to do). Bench diagnostics. */
boolean OTAAPP_confirmed(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_APP_H */
