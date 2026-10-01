/*
 * sbl_boot.h - SBL boot flow glue (doc 24 §5.1): metadata -> decision -> jump
 */
#ifndef SBL_BOOT_H
#define SBL_BOOT_H

#include "Ifx_Types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The whole SBL payload after core0_main's watchdog handling. Loads the
 * OtaMeta double page, runs the OTABOOT ladder (committing any metadata
 * transition it produces), signals the decision on the LED and jumps into
 * the chosen slot's entry. Never returns: the no-slot path parks in a
 * blinking safe state (doc 24 §5.1 "无有效槽 -> 停在 SBL 安全态"). */
void SBL_boot(void);

/* Jump to a slot's App entry (slot base + 0x20, the ".start" section the
 * App linker placed there - ota_layout.h). Disables interrupts first; the
 * App's own CStart re-initializes stacks, PSW, BTV/BIV, data and clocks,
 * then releases CPU1/2 (its Ifx_Cfg is the multi-core one; the SBL's is
 * not). Never returns. */
void SBL_jumpToSlot(uint8 slot);

#ifdef __cplusplus
}
#endif

#endif /* SBL_BOOT_H */
