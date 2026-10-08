/* calib_store.h - CPU0 bench-calibration persistence (doc 34 SS8).
 *
 * DFlash0 sector 15 (0xAF01E000) holds one CalibRecord (single slot, no wear
 * levelling; magic/CRC/range validated on load). SDD SS4.2 reserves the rest
 * of DF0 for the §4.3 config pages, which must not claim sector 15.
 * CPU0 is the ONLY core that touches the flash controller here, and
 * only from the robot control task (10 ms tick): the write sequence feeds
 * the CPU watchdog, masks this core's interrupts, erases, programs, reads
 * back. A full erase+program is tens of ms (bench-only frequency: <=10 per
 * day, DFlash cycle life >=1e5 -> no wear levelling, doc 34 SS8.1).
 *
 * Writes are deferred while the vehicle is moving (doc 34 SS8.3): tick()
 * performs them once targets, jog duties and measured speeds are all zero.
 *
 * Call from CPU0 only:
 *   CALIB_init()             boot, before the scheduler (loads + publishes)
 *   CALIB_tick()             robot control task, every 10 ms: drains CPU1's
 *                            result mailbox (EVT 0x22, auto-persist on DONE)
 *                            and runs the deferred-write queue
 *   CALIB_recordSet()        0x73: validate, apply, echo, queue write
 *   CALIB_recordClear()      0x74: erase, defaults, echo
 */
#ifndef CALIB_STORE_H
#define CALIB_STORE_H

#include "Ifx_Types.h"
#include "mw/calib/calib_record.h"

/* Load the DFlash record and publish it (defaults on any validation
 * failure, reported with src=CALIB_SRC_DEFAULT and crcOk=0). */
void CALIB_init(void);

/* Drive the result mailbox and the deferred-write queue. ~10 ms cadence,
 * CPU0's robot control task. */
void CALIB_tick(void);

/* 0x72 REC_GET: echo the live record on EVT 0x23. */
void CALIB_sendRecord(void);

/* 0x73 REC_SET: decode 12 B body, validate, apply live, echo EVT 0x23,
 * queue the DFlash write. Invalid bodies are rejected without touching the
 * live record. */
void CALIB_recordSet(const uint8 *data, uint8 len);

/* 0x74 REC_CLEAR: publish defaults, queue the sector erase (echo already
 * covered by the caller's EVT 0x23). */
void CALIB_recordClear(void);

/* 0x7A: parked-only, fresh-IMU axis/track calibration. Applies via the live
 * record; CPU0 fusion task sees it on the next tick. EVT 0x23 saved byte
 * reports pending/success/failure/rejection. */
void CALIB_imuSet(const uint8 *data, uint8 len);

#endif /* CALIB_STORE_H */
