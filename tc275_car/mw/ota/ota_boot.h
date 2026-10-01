/*
 * ota_boot.h - SBL boot decision + App-side state confirmations (doc 24 §5)
 *
 * Pure logic over an OtaMeta record: the SBL feeds it the loaded metadata,
 * gets back which slot to jump into (or safe mode) plus a mutated record to
 * commit. Runs identically in the SBL and in the host tests, so the rollback
 * ladder of doc 24 §5.1 is testable without hardware (gate G-OTA-5, logic
 * level; the on-board half stays manual).
 *
 * One deliberate refinement over doc 24 §5.1 branch 3: a slot already in
 * VALID boots WITHOUT rewriting the metadata. The document has the SBL
 * commit boot_attempts++ on every boot, which would burn one DFlash erase
 * cycle per power cycle for no decision-relevant state; attempts only mean
 * anything while the active slot is PENDING_VERIFY, so the write is skipped
 * exactly when it cannot change a future decision. (Doc updated accordingly.)
 */
#ifndef OTA_BOOT_H
#define OTA_BOOT_H

#include <stdint.h>
#include "ota_meta.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Boot attempts budget for a freshly swapped-in image before the SBL gives
 * up and rolls back (doc 24 §5.1 threshold, "如3"). */
#define OTABOOT_MAX_ATTEMPTS    3u

typedef enum
{
    OTABOOT_JUMP_A = 0,     /* boot slot A                                */
    OTABOOT_JUMP_B,         /* boot slot B                                */
    OTABOOT_SAFE_MODE       /* stay in the SBL, signal via LED            */
} OtaBootAction;

/* The §5.1 ladder, evaluated top to bottom:
 *   1. pending slot fully downloaded  -> activate it as PENDING_VERIFY, jump
 *   2. active PENDING_VERIFY exhausted -> roll back to the other slot if it
 *      is VALID (marking the failed one INVALID), else safe mode
 *   3. active PENDING_VERIFY (tries left)  -> attempts++, jump
 *   4. active VALID                       -> jump, no metadata write
 *   5. anything else                      -> safe mode
 *
 * firstBootAOk covers the factory bring-up path: when no valid metadata page
 * exists at all (blank DFlash), the SBL boots slot A iff its entry looks
 * programmed (see sbl/FLASHOTA_slotEntrySane) without writing metadata.
 *
 * Returns 1 when *m was mutated and the caller must OTAMETA_commit it. */
uint8_t OTABOOT_decide(OtaMeta *m, uint8_t firstBootAOk, OtaBootAction *out);

/* App-side: the new image ran its self-checks and may be kept (§5.2).
 * PENDING_VERIFY -> VALID, attempts reset, pending cleared. Returns 1 when
 * the record changed (caller commits); 0 when there was nothing to do. */
uint8_t OTABOOT_confirmSelftest(OtaMeta *m);

/* Receiver-side helpers used by ota_rx's metadata callbacks. */
uint8_t OTABOOT_markDownloaded(OtaMeta *m, uint8_t slot);   /* state=DOWNLOADED, pending=slot */
uint8_t OTABOOT_markAborted(OtaMeta *m, uint8_t slot);      /* state=EMPTY, pending cleared   */

/* Slot the given record would boot, or OTA_SLOT_NONE when it would not. */
uint8_t OTABOOT_bootSlot(const OtaMeta *m);

#ifdef __cplusplus
}
#endif

#endif /* OTA_BOOT_H */
