#ifndef STIME_H
#define STIME_H

#include "Ifx_Types.h"

/* Millisecond timebase for the bare-metal cores (CPU1 motor loop, CPU2 WiFi).
 * Reads the free-running STM0 counter, so no timer setup is required and the
 * value is identical on every core. All comparisons use unsigned arithmetic
 * and are therefore wrap-safe.
 * Note: core 0 runs FreeRTOS and uses tick counts instead of this module. */

void   STIME_init(void);
uint32 STIME_nowMs(void);
void   STIME_waitUntilMs(uint32 ms);   /* busy wait until the given absolute ms (dedicated loops only) */
void   STIME_delayMs(uint32 ms);       /* busy wait for a relative duration */

#endif
