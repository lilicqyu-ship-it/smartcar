#ifndef TOF_H
#define TOF_H

#include "Ifx_Types.h"

/* VL53L5CX 8x8 multizone ToF, CPU0 (doc 30-tc275/36-tof-driver.md, wiring
 * truth source doc/20-design/23-wiring.md section 11).
 *
 * Bus: I2C0 hardware master, SCL=P02.5 / SDA=P02.4 (X304 holes 7/8),
 * 1 MHz requested for the bench trial, 8-bit slave address 0x52. Register addressing is 16-bit: the first
 * byte after the slave address carries the unmodified high index byte, the
 * second the low byte, then the payload (DS13754, and the same convention the
 * ST Ultra Lite Driver's platform contract assumes).
 *
 * Ownership: CPU0 only (the FreeRTOS service core). The bus is polled, never
 * interrupt driven, and no other core may touch I2C0. The 84 KB device
 * firmware download and every frame read run inside vTofTask, which sits one
 * priority BELOW the robot task, because a single WrMulti of 32 KB takes about
 * 2.2 s of bus time at 400 kHz and must never hold off the watchdog feed.
 *
 * INT (P10.7, mikroBUS hole 15) is level-polled only: it shares ERU input
 * channel 0 with the IMU's INT1, so edge interrupts are not available to this
 * device at all (doc 23 section 11.2). The level is read as bench evidence.
 *
 * Data path: all register semantics come from ST's ULD (Libraries/ST/vl53l5cx),
 * which is shipped verbatim. Nothing here re-implements a register table -
 * the driver only owns the wire format, the transaction bounds, and the state
 * machine. Each complete ranging frame is published as a coherent FusionTof
 * snapshot through xcore, including target counts/statuses and a timestamp.
 * CPU0 fusion uses freshness independently of the driver alive flag. */

/* ---- device constants taken from the vendor contract, not hand-derived ----
 * 0x52 is VL53L5CX_DEFAULT_I2C_ADDRESS (vl53l5cx_api.h:35) in the 8-bit form
 * iLLD expects (7-bit 0x29 left-shifted by one). */
#define TOF_I2C_ADDRESS         0x52u

/* ---- ranging configuration switch ----
 * The first bench pass runs 4x4 (doc/20-design/23-wiring.md section 11.4 step
 * 6): a third of the per-frame bus time and less than a third of the RAM of
 * 8x8, which is the right trade while the wiring is still unverified. Set to 1
 * for the 8x8 pass. One switch moves the sensor resolution and the zone-count
 * bound of the getters together, so the two can never disagree. */
#define TOF_CFG_8X8             0

#if (TOF_CFG_8X8 != 0)
#define TOF_ZONE_COUNT          64u     /* VL53L5CX_RESOLUTION_8X8 (api.h:44) */
#else
#define TOF_ZONE_COUNT          16u     /* VL53L5CX_RESOLUTION_4X4 (api.h:43) */
#endif

/* ---- chunking (transaction bounds, see doc 36 section 4) ----
 * The I2C0 TX and RX FIFOs are each 8 words deep, and a transaction is one
 * session with an explicit 16-bit index, so both directions are cut into
 * chunks that fit one FIFO. Write chunks carry the slave-address byte, the
 * index pair and this many payload bytes; read chunks stay at or below the
 * 32-byte case iLLD documents as safe to complete inside the FIFO
 * (IfxI2c_I2c.h:48-49) - above it the vendor driver disables global
 * interrupts for the whole burst, which a FreeRTOS service task must never do.
 * TX packets are packed first, then queued in a bounded register-only critical
 * section (at most eight words); RX requests are serviced per word. All
 * protocol/space/request waits and the wire transfer keep interrupts enabled. */
#define TOF_TX_MAX              32u     /* write message bytes, address included */
#define TOF_WRITE_CHUNK         24u
#define TOF_READ_CHUNK          32u

/* Per-message deadline. 27 B is ~0.7 ms at 400 kHz, so this only ever fires
 * on a stuck clock or a dead module - it is a hardware-fault detector, not a
 * tuning knob. */
#define TOF_XFER_TIMEOUT_MS     20u

/* ---- wire format (pure, host-tested) ---- */

/* Slave address byte: bit0 = RnW. The vendor hands over the 8-bit write
 * address, so mask/set explicitly instead of trusting the caller's low bit. */
static inline uint8 TOF_slaveAddrWrite(uint8 addr8)
{
    return (uint8)(addr8 & 0xFEu);
}

static inline uint8 TOF_slaveAddrRead(uint8 addr8)
{
    return (uint8)(addr8 | 0x01u);
}

/* 16-bit register index: high byte first, unchanged for reads and writes.
 * RnW belongs only to the slave address byte (0x52/0x53). */
static inline uint8 TOF_wireAddrHiWrite(uint16 reg)
{
    return (uint8)(reg >> 8);
}

static inline uint8 TOF_wireAddrHiRead(uint16 reg)
{
    return (uint8)(reg >> 8);
}

static inline uint8 TOF_wireAddrLo(uint16 reg)
{
    return (uint8)(reg & 0xFFu);
}

/* Bytes put on the bus after the slave address: the index pair + payload. */
static inline uint16 TOF_wireBytes(uint16 dataLen)
{
    return (uint16)(dataLen + 2u);
}

/* Total bytes of one write message, slave-address byte included: that is the
 * number the hardware packet-size register is set to, and it must stay within
 * TOF_TX_MAX or the message no longer fits the TX FIFO. */
static inline uint16 TOF_txMessageBytes(uint16 dataLen)
{
    return (uint16)(TOF_wireBytes(dataLen) + 1u);
}

/* The page register (0x7FFF) re-maps a 32 KB window, and the index is 16-bit,
 * so a burst must never run past 0xFFFF inside one page: WrMulti of 0x8000
 * bytes from 0x0000 is exactly the ceiling. Anything that would wrap is a
 * caller bug and has to fail before the first message, not half-send. */
static inline boolean TOF_indexFits(uint16 base, uint32 total)
{
    return ((uint32)base + total) <= 65536u;
}

/* ---- chunk arithmetic (pure, host-tested) ----
 * chunk > 0 and total >= chunk*index are guaranteed by the callers, which all
 * loop over TOF_chunkCount(total, chunk). */
static inline uint32 TOF_chunkCount(uint32 total, uint16 chunk)
{
    return (total + (uint32)chunk - 1u) / (uint32)chunk;
}

static inline uint16 TOF_chunkAddr(uint16 base, uint32 index, uint16 chunk)
{
    return (uint16)(base + (uint32)((uint32)index * (uint32)chunk));
}

static inline uint16 TOF_chunkLen(uint32 index, uint32 total, uint16 chunk)
{
    uint32 start = index * (uint32)chunk;
    uint32 remain = total - start;

    return (remain < (uint32)chunk) ? (uint16)remain : chunk;
}

/* ---- result interpretation ---- */

/* Target status values that mean "this measurement ranged OK", quoted from
 * the vendor's own field documentation (vl53l5cx_api.h:343: "5 & 9 means
 * ranging OK"). The consumer's own thresholds are later work; this is only
 * the driver's liveness criterion for logging and for the failure run. */
static inline boolean TOF_targetStatusOk(uint8 status)
{
    return ((status == 5u) || (status == 9u)) ? TRUE : FALSE;
}

/* ---- status codes ---- */
typedef enum
{
    TOF_OK = 0,
    TOF_ERR_PARAM,        /* bad length / index would wrap the 16-bit space */
    TOF_ERR_NO_ACK,       /* slave address not acknowledged (absent, LPn low) */
    TOF_ERR_BUS,          /* arbitration lost or module error flag          */
    TOF_ERR_TIMEOUT,      /* message never finished, module re-armed        */
    TOF_ERR_ST            /* the ULD returned a non-zero status             */
} TofStatus;

/* Task cadence: vTofTask calls TOF_task() this often. 5 ms is well inside the
 * 66 ms frame period at 15 Hz and cheap enough to stay a rounding error on
 * CPU0's load; the state machine does the rate shaping itself. */
#define TOF_TASK_PERIOD_MS      5u

/* ---- driver API, CPU0 only ---- */
void    TOF_init(void);    /* I2C0 + INT pin + probe, state machine starts dead */
void    TOF_task(void);    /* call every TOF_TASK_PERIOD_MS from vTofTask only  */

boolean TOF_isAlive(void); /* ranging is up and the last frame was clean     */
uint32  TOF_errCount(void);   /* failed I2C messages since boot              */
uint32  TOF_frameCount(void); /* accepted frames since boot                  */
uint32  TOF_initMs(void);     /* measured vl53l5cx_init() cost, bench proof  */

uint16  TOF_zoneCount(void);  /* 4x4 or 8x8, whatever is currently configured */
sint16  TOF_distanceMm(uint8 zone);
uint8   TOF_targetStatus(uint8 zone);
uint8   TOF_intLevel(void);   /* P10.7 as polled: 1 = frame pending          */

#endif /* TOF_H */
