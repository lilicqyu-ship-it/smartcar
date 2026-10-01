/*
 * flash_ota.h - PFlash/DFlash erase+program backend on top of iLLD IfxFlash
 * (doc 24 §7), TASKING/TC275 only
 *
 * Responsibilities, in the layering of doc 24:
 *   - slot erase/write/read for ota_rx (PFlash, the non-running bank)
 *   - the DFlash backend of the OtaMeta double-page store
 *   - the slot-entry sanity probe the SBL uses before its first-boot path
 *
 * Hardware rules this honors (violating any of them costs ECC errors or a
 * wedged FMU, doc 24 §7):
 *   - program page is 32 B (PFlash) / 8 B (DFlash), written whole, never
 *     twice (ECC); partial staging lives in RAM here
 *   - erase unit is the logical sector; slot erase walks the iLLD sector
 *     table for the slot's bank
 *   - all flash COMMANDS go to non-cached addresses (0xA0../0xAF..)
 *   - erase/program of one bank stalls fetches from THAT bank only; the OTA
 *     receiver always writes the bank it is not executing from. The command
 *     issue itself is a handful of register writes done with interrupts
 *     masked; the busy-wait afterwards runs with interrupts enabled so the
 *     link pump and watchdog service keep running during multi-100-ms erases
 *   - every operation polls the matching FSR busy bit with a timeout and
 *     checks the OPER/PROER error bits afterwards
 *
 * The SBL calls this with interrupts globally disabled already; the App
 * calls it from the CPU2 link context (doc 24 §5.3).
 */
#ifndef FLASHOTA_H
#define FLASHOTA_H

#include <stdint.h>

#include "Ifx_Types.h"
#include "../mw/ota/ota_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- slot (PFlash) operations ------------------------------------------------ */

/* Erase every logical sector of the slot (PF S2..S26 of its bank).
 * slot: OTA_SLOT_A / OTA_SLOT_B. Returns TRUE on success. */
boolean FLASHOTA_eraseSlot(uint8 slot);

/* Sequential image write into the (erased) slot. off must equal the number
 * of bytes written so far (the staging page enforces it); len arbitrary.
 * Returns TRUE on success. */
boolean FLASHOTA_writeApp(uint8 slot, uint32 off,
                          const uint8 *data, uint32 len);

/* Program the staged tail (padded with 0xFF). written = total image bytes.
 * Returns TRUE on success. */
boolean FLASHOTA_flushApp(uint8 slot, uint32 written);

/* Plain read-back from the slot (non-cached alias). Returns TRUE. */
boolean FLASHOTA_readApp(uint8 slot, uint32 off, uint32 len, uint8 *out);

/* 1 when the slot's entry (slot base + 0x20) does not read as erased or
 * blank flash - the SBL's guard for the no-metadata first boot. */
uint8 FLASHOTA_slotEntrySane(uint8 slot);

/* ---- OtaMeta DFlash backend ---------------------------------------------------- */

/* Wires FLASHOTA into the OTAMETA double-page store. */
void FLASHOTA_metaAttach(void);

/* Exposed for the host mock parity and for App-side glue. */
boolean FLASHOTA_metaErase(uint8 pageIdx);
boolean FLASHOTA_metaWrite(uint8 pageIdx, const uint8 *data, uint32 len);
void    FLASHOTA_metaRead(uint8 pageIdx, uint8 *data, uint32 len);

#ifdef __cplusplus
}
#endif

#endif /* FLASHOTA_H */
