#ifndef ADC_H
#define ADC_H

#include "Ifx_Types.h"

/* Battery voltage acquisition, CPU1.
 *
 * Signal chain (doc/20-design/23-wiring.md section 6/9): the D24A board
 * divides its VIN (the drive battery, 2S..3S => 6.5..12.6 V) with R13 10k
 * series / R15 1k shunt and brings the node out on J6-1 ("ADC", VIN/11).
 * That node is wired to kit X2-23 = analog pad AN4 = VADC group 0 channel 4
 * (kit manual Figure 4; the whole X2-16..27 column is free analog pads,
 * X2-27/AN0 is the board potentiometer).
 *
 * Conversion: polled one-shot through the iLLD queue, 12 bit, default 1 us
 * sample time - generous for the ~909 ohm Thevenin source of the divider.
 * wait-for-read mode clears the valid flag on read, so one filtered sample
 * per ADC_TASK_PERIOD_MS is a straight add/queue/read sequence.
 *
 * Calibration constants live here and here only (bench-verify with a
 * multimeter before trusting the absolute value):
 *   ADC_BATT_VAREF_MV - the kit's VAREF1 rail; X2-15 exposes it. The kit
 *                       powers the analog rail from VEXT (3.3 V default);
 *                       VAREF accuracy scales the whole reading.
 *   ADC_BATT_DIV_NUM  - divider ratio x1000: VIN = pin * 11 (10k+1k)/1k.
 *                       VDDM/LOSUP: the iLLD supply-voltage select is set to
 *                       3.3 V to match this board; flip together with
 *                       VAREF_MV if the analog rail is ever moved to 5 V. */

#define ADC_BATT_VAREF_MV       3300u   /* kit VAREF1 rail (X2-15), bench-verify */
#define ADC_BATT_DIV_NUM        11u     /* VIN = pin_mV * (10k+1k)/1k            */
#define ADC_BATT_CELLS          3u      /* percent math, doc 23 SS6: 2S..3S;
                                         * SDD SS16 Q1: config item pending     */

/* Li-ion window used for batteryPct only (4.2 V full / 3.3 V empty per cell) */
#define ADC_CELL_FULL_MV        4200u
#define ADC_CELL_EMPTY_MV       3300u

void   ADC_init(void);                   /* CPU1, once, before the first ADC_task */
void   ADC_task(void);                   /* CPU1: one conversion + EMA filter     */
uint16 ADC_battVinMv(void);              /* last filtered VIN, mV (CPU1's copy)   */
uint8  ADC_battPct(void);                /* CPU1: charge estimate of own reading  */
uint8  ADC_battPctFromMv(uint16 vinMv);  /* pure math, safe on ANY core (CPU2
                                          * telemetry uses this over the xcore
                                          * mV - the CPU1 statics do not exist
                                          * in another core's address space)   */

#endif /* ADC_H */
