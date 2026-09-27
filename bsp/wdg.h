#ifndef WDG_H
#define WDG_H

/* CPU watchdog helper for whichever core includes it (doc 21 SS18 C8).
 *
 * Timeout base: Infineon's official watchdog example for this board
 * (AURIX_code_examples / Watchdog_1_KIT_TC275_LK) states CPU-WDT
 * reload 0xE000 is ~1.3 s with the power-on watchdog clock. Counting is
 * linear in the reload, so WDG_CPU_REL = 0xF800 (half the countdown
 * steps) is ~0.3..0.5 s: far above the longest service interval of any
 * core here (CPU0: 10 ms robot task, CPU1: 1 kHz loop) and short enough
 * that a stuck core resets the vehicle within ~0.5 s instead of driving
 * on stale PWM. Exact value to be measured on the bench into the
 * verification log (SDD SS7.1 targets <=200 ms; that needs a smaller
 * REL, safe to tighten once measured).
 *
 * The service is an ENDINIT clear+set pair, not
 * IfxScuWdt_serviceCpuWatchdog(): ENDINIT is a saturating counter and a
 * pure-set service increments it on every refresh, pinning it at 15
 * within 16 services - after that no single clear can unlock ENDINIT
 * again and the flash/OTA paths (SDD SS9) would hang. The set step of
 * the pair performs the actual refresh/reload (same pattern iLLD uses
 * inside IfxScuCcu_init).
 *
 * Debugger note: while a debugger is attached, TriCore OCD suspends the
 * watchdog on core halt, so this does not break flash debugging; the
 * bite only happens in standalone runs, which is the point.
 */

#include "IfxScuWdt.h"

/** \brief CPU-WDT reload value: ~0.3..0.5 s timeout (see header note) */
#define WDG_CPU_REL 0xF800u

/* Refresh this core's CPU watchdog. Must be called at least once per
 * WDG_CPU_REL window; the per-core contracts are:
 *   CPU0: vRobotControlTask loop (10 ms)   CPU1: MOTOR_ALGO_run (1 ms) */
static inline void WDG_serviceCpu(void)
{
    uint16 pw = IfxScuWdt_getCpuWatchdogPassword();

    IfxScuWdt_clearCpuEndinit(pw);
    IfxScuWdt_setCpuEndinit(pw);
}

/* Set this core's CPU watchdog to WDG_CPU_REL and start the service
 * contract with an immediate refresh. Call once at core entry, replacing
 * the template's IfxScuWdt_disableCpuWatchdog(). */
static inline void WDG_enableCpu(void)
{
    IfxScuWdt_changeCpuWatchdogReload(IfxScuWdt_getCpuWatchdogPassword(), WDG_CPU_REL);
    WDG_serviceCpu();
}

#endif /* WDG_H */
