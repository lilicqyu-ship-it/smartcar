/*
 * link.h - SF-over-SPI link pump, TC275 (CPU2) master side
 *
 * Wire truth source: doc/20-design/22-link-spi-design.md SS4.2 (the pump
 * sequence), SS4.3 (shared register map), SS5 (SF frame) and SS6 (budgets).
 * Layers below: com/spi_hal_pins.c (one raw half duplex
 * transaction) and mw/sf/sf_frame.c (frame codec).
 *
 * This is the production LINK. The demo UART path (com/wifi_at.c) stays
 * the build default until gate G1 passes, so Cpu2_Main.c mounts one or the
 * other behind USE_SPI_LINK.
 */
#ifndef LINK_H
#define LINK_H

#include "Ifx_Types.h"

#include "mw/proto/protocol.h"
#include "com/spi_hal_pins.h"
#include "mw/sf/sf_frame.h"
#include "mw/sf/sf_telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- shared register file (22 SS4.3), seven little endian u32 --------------
 * Addresses are byte offsets into the slave's 64 byte buffer RAM, which is how
 * Espressif's own master driver addresses it (essl_spi.c: addr % 72).
 * The slave publishes READY..GEN's neighbours, i.e. 28 bytes; the master only
 * ever reads the first 24, because GEN is its own write slot. */
#define LINK_REG_READY        0u
#define LINK_REG_TX_PENDING   4u
#define LINK_REG_RX_ROOM      8u
#define LINK_REG_ALIVE        12u
#define LINK_REG_ERRSTAT      16u
#define LINK_REG_CMDRSP       20u
#define LINK_REG_GEN          24u     /* master -> slave command slot (WRBUF)  */
#define LINK_REG_BYTES        24u     /* read window: READY .. CMDRSP          */
#define LINK_REG_PUBLISHED    28u     /* what the slave actually maintains     */

#define LINK_READY_MAGIC      0x5F534601u  /* "_SF1" as written in 22 SS4.3 */

/* ---- GEN commands, written to LINK_REG_GEN as {u8 cmd, u8 p0, u8 p1, u8 p2}
 * and answered in LINK_REG_CMDRSP as {u8 cmd, u8 result} (22 SS4.3). The slave
 * handles a GEN write on its buffer-written event (c6_link/link.c:
 * link_handle_gen), so this is the one thing the master can ask the slave for
 * without a frame - which is why gates G3 (drop the IRQ) and G4 (silence it)
 * are injected through it rather than through a scripted C6 build. */
#define LINK_GEN_NOP             0u
#define LINK_GEN_RESET_LINK      1u   /* reset the slave's link state machines */
#define LINK_GEN_SILENCE_ON      2u   /* stop asserting IRQ (bench injection)  */
#define LINK_GEN_SILENCE_OFF     3u
#define LINK_GEN_CLOCK_SET       4u   /* payload = clock in MHz: the slave
                                       * multiplies by 1e6 (link_handle_gen), so
                                       * 5 means the 5 MHz rung of the ladder   */

#define LINK_GEN_RESULT_OK       0u
#define LINK_GEN_RESULT_UNKNOWN  1u

/* How long a GEN receipt is waited for before the slave is assumed not to have
 * acted. Its link task wakes on the SPI buffer-written event, so the normal
 * figure is well under a millisecond; 20 ms also covers the 10 ms ALIVE timer
 * period it can be interleaved with. */
#define LINK_GEN_TIMEOUT_MS      20u

/* Slave heartbeat stalls for longer than this = the slave is gone (22 SS5.4).
 * The alarm has to fire before the 520 ms link-loss gate G4 allows. */
#define LINK_ALIVE_TIMEOUT_MS 500u

/* Floor for the register poll when the slave's IRQ line is quiet (22 SS4.1).
 * P23.0 is polled, not interrupt driven: P23.x has no edge interrupt on this
 * package. */
#define LINK_KEEPALIVE_MS     2u

/* Consecutive unstable register snapshots before the slave's state is declared
 * untrustworthy (the non atomic read rule, 22 SS4.3 / E5). */
#define LINK_REG_RETRY_MAX    3u

/* TX queue capacity: frames are packed one after another into a segment, so
 * this is the number of frames that may wait for bus room. */
#define LINK_TXQ_DEPTH        4u

/* Command payloads on this link are the demo command set (22 SS5.5 keeps them
 * byte compatible), so anything wider cannot be forwarded to CPU0. */
#define LINK_CMD_DATA_MAX     PROTO_MAX_PAYLOAD

/* Slave side error bits published in LINK_REG_ERRSTAT. These are the slave's
 * own definitions (c6_car components/c6_sf/sf_frame.h SF_ERR_*), each sticky
 * with a saturating counter behind it; 22 SS4.3 keeps the same table. */
#define LINK_ERRSTAT_SLAVE_CRC       0x00000001u
#define LINK_ERRSTAT_SLAVE_FMT       0x00000002u
#define LINK_ERRSTAT_SLAVE_SEQ       0x00000004u
#define LINK_ERRSTAT_SLAVE_RXOVFL    0x00000008u  /* RX queue overflow, frame lost */
#define LINK_ERRSTAT_SLAVE_TXOVFL    0x00000010u  /* TX queue overflow (BUSY)      */
#define LINK_ERRSTAT_SLAVE_TRUN      0x00000020u  /* residual frame across segs    */
#define LINK_ERRSTAT_SLAVE_LINKLOST  0x00000040u  /* 5 consecutive CRC failures    */

typedef enum
{
    LINK_DOWN = 0,      /* no SF_READY seen: data transactions are forbidden */
    LINK_READY,         /* registers readable, link usable */
    LINK_LOST           /* was READY and the heartbeat stopped: stop requested */
} Link_State;

typedef struct
{
    uint32 polls;             /* register snapshot transactions */
    uint32 regRetries;        /* snapshot needed another pass (E5) */
    uint32 regUnstable;       /* snapshot still unstable after LINK_REG_RETRY_MAX */
    uint32 rdSegments;        /* RDDMA segments taken from the slave */
    uint32 rdBurstClamped;    /* TX_PENDING above LINK_RX_BURST_MAX: register
                               * value distrusted, burst shortened */
    uint32 wrSegments;        /* WRDMA segments pushed to the slave */
    uint32 txFrames;          /* SF frames put on the bus */
    uint32 txDropped;         /* slot dropped by the codec (must stay 0) */
    uint32 txQueueFull;       /* LINK_send() refused: backpressure seen */
    uint32 rxFrames;          /* SF frames accepted by the codec */
    uint32 crcErrors;
    uint32 seqErrors;
    uint32 cmdForwarded;      /* CMD frames handed to the CPU0 queue */
    uint32 cmdRejectedQueue;  /* CPU0 queue full: command dropped (must stay 0) */
    uint32 cmdOversize;       /* payload wider than LINK_CMD_DATA_MAX (must stay 0) */
    uint32 cmdBadLen;         /* payload shorter than the CID's own minimum: the
                               * frame does not describe what it claims, so it is
                               * not executed (22 SS5.5) */
    uint32 cmdUnsupportedCid; /* TYPE_CMD of a CID this build has no consumer for;
                               * executing it would run configuration or pairing
                               * bytes as if they were a command */
    uint32 cmdUnsupportedOp;  /* a known channel whose command this build cannot
                               * execute (0x50 DRIVE until the speed loop exists) */
    uint32 unhandledType;     /* RX frame of a type this build does not consume */
    uint32 genWrites;         /* GEN transactions issued to the slave */
    uint32 genNoAck;          /* GEN write whose receipt did not come back as
                               * {same cmd, result OK}: the slave did not act */
    uint32 spiErrors;         /* transactions that did not complete cleanly */
    uint32 linkLost;          /* falling edges into LINK_LOST */
} Link_Stats;

typedef struct
{
    Link_State      state;
    uint32          sinceAliveMs;   /* ms since LINK_REG_ALIVE last advanced */
    uint32          txPending;      /* slave's claim, as last measured */
    uint32          rxRoom;
    uint32          slaveErrStat;
    SpiHal_ClockTier clock;
    uint32          clockHz;        /* post divider, i.e. what is on the wire */
    Link_Stats      stats;
    SF_Stats        frameStats;
    SpiHal_Stats    spi;            /* transaction level counters, kept separate
                                     * so it is obvious which layer failed */
} Link_Health;

/* Bring the SPI master up and start in LINK_DOWN. Requires STIME_init(). */
void LINK_init(SpiHal_ClockTier tier);

/* Pump one link cycle: snapshot registers, drain the slave, feed our own queue.
 * Call from the CPU2 superloop as often as possible. Blocks for at most one
 * segment on the wire (263 bytes = 2.1 ms at the 1 MHz rung), which is fine for
 * a core whose only job is this link. Never call from an ISR. */
void LINK_main(void);

/* Queue one SF frame for the next write transaction. Returns FALSE - and counts
 * stats.txQueueFull - when the TX queue is full, which is the backpressure
 * signal the caller must act on rather than lose
 * (22 SS5.2 "commands are not dropped: queue full means backpressure + alarm"). */
boolean LINK_send(uint8 type, uint8 cid, const uint8 *payload, uint8 len);

/* Queue one telemetry frame (SF_TYPE_TEL / SF_CID_TELEMETRY).
 *
 * The payload is the fixed 38 byte proto v2 0x41 layout from
 * mw/sf/sf_telemetry.h, and the length is not negotiable: the slave
 * drops every TEL frame that is not exactly that CID with at least
 * SF_TELEMETRY_LEN bytes (c6_car components/c6_link/link.c:sf_to_v2), so a short
 * telemetry payload is not "less information", it is no information.
 *
 * This call owns the E2E sequence field, so a producer that hands over a filled
 * SF_Telemetry gets a consistent seq stream even if it forgets to count. Every
 * field with no measured source in this build must be written as zero by the
 * caller rather than filled with a value in another unit - the C6 forwards these
 * bytes to the phone, which labels them mm/s, mV and mm.
 *
 * Returns FALSE with stats.txQueueFull when the TX queue has no room. */
boolean LINK_sendTelemetry(const SF_Telemetry *tel);

/* TRUE while the link can carry commands (22 SS5.4: the caller decides whether
 * a frame is worth queueing at all). */
boolean LINK_isUp(void);

/* ---- register level control of the slave (22 SS4.3 GEN slot) ---------------
 * One GEN transaction: WRBUF {cmd, p0, p1, p2} into LINK_REG_GEN, then read the
 * register window back until LINK_REG_CMDRSP answers {cmd, result}. The slave
 * handles GEN in its link task, not in the SPI ISR, so the receipt can lag the
 * write by its scheduling interval; timeoutMs bounds that wait (a few ms is
 * enough on a healthy slave, 20 ms covers a busy one).
 *
 * This is what gates G3 (drop the IRQ) and G4 (slave goes silent) inject
 * through: LINK_GEN_SILENCE_ON/OFF stop the slave from asserting P23.0 without
 * touching anything else, which is exactly the failure R11 has to be measured
 * against. Call from the CPU2 superloop between LINK_main() calls, never from an
 * ISR, never while the link is not READY. */
typedef enum
{
    LINK_GEN_OK = 0,
    LINK_GEN_ERR_SPI,     /* the write or a read-back transaction failed */
    LINK_GEN_ERR_NOACK,   /* CMDRSP never came back carrying this command */
    LINK_GEN_ERR_REJECT,  /* the slave answered LINK_GEN_RESULT_UNKNOWN */
    LINK_GEN_ERR_RANGE    /* caller passed a payload wider than the 24-bit slot;
                           * nothing went on the wire - a programming error, not
                           * a link fault, so it must not read as ERR_SPI */
} Link_GenResult;

Link_GenResult LINK_gen(uint8 cmd, uint32 payload, uint32 timeoutMs);

/* Move the master to another rung of the clock ladder and mirror the move to the
 * slave with LINK_GEN_CLOCK_SET. Safe to call only between LINK_main() calls,
 * while no transaction is in flight (22 SS3.2). The mirror is diagnostic on the
 * slave side, so a refused mirror does not roll the local change back - it shows
 * up in stats.genNoAck instead. Returns the SPIHAL status of the local switch. */
SpiHal_Status LINK_setClock(SpiHal_ClockTier tier);

void LINK_getHealth(Link_Health *health);

#ifdef __cplusplus
}
#endif

#endif /* LINK_H */
