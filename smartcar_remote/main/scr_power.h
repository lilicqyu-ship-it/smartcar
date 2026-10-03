/*
 * scr_power.h - auto power-off: touch inactivity -> countdown -> deep sleep
 *
 * 关机 on this hardware: the EV board has no soft power latch, no PMIC kill
 * line and the GT1151 INT pin is not wired, so touch cannot wake from sleep.
 * The only honest power-off is deep sleep, and the only wake source is the
 * BOOT button (GPIO0, ext0 low level).  A deep-sleep wake reboots through the
 * normal boot path = 开机.
 */
#ifndef SCR_POWER_H
#define SCR_POWER_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start the inactivity watchdog.  Must run in LVGL context (call it under
 * the display lock, next to ui_init).  No-op when CONFIG_SCR_AUTO_OFF_ENABLE=n
 * and never started in BENCH build. */
void scr_power_start(void);

#ifdef __cplusplus
}
#endif

#endif /* SCR_POWER_H */
