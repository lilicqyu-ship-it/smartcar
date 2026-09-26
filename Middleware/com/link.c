/*
 * link.c - SF-over-SPI link pump, TC275 (CPU2) master side
 *
 * Implements the cycle of doc/20-design/22-link-spi-design.md SS4.2 in the order
 * the document fixes it: register snapshot, then read (higher priority), then
 * write, then liveness. Owner core: CPU2, bare metal, superloop context only.
 */
#include "link.h"

#include "xcore.h"
#include "stime.h"

#include <string.h>

/* A snapshot whose ALIVE / CMDRSP fields differ between the two passes is not
 * an error: the slave is allowed to advance those at any time. The four fields
 * below are the ones a decision is taken from, so they must be stable. */
static const uint8 g_stableRegOfs[4] = {
    LINK_REG_READY, LINK_REG_TX_PENDING, LINK_REG_RX_ROOM, LINK_REG_ERRSTAT
};

/* TX_PENDING above this cannot be a real buffer claim (the slave's whole buffer
 * is smaller), so it is treated as a bad register read and the drain loop is
 * shortened. 640 B is two and a half maximum segments. */
#define LINK_RX_BURST_MAX     640u

typedef struct
{
    uint8 type;
    uint8 cid;
    uint8 len;
    uint8 payload[SF_MAX_PAYLOAD];
} Link_TxSlot;

static Link_TxSlot  g_txq[LINK_TXQ_DEPTH];
static uint8        g_txHead;
static uint8        g_txTail;
static uint8        g_txSeq;

static SF_Parser    g_rxParser;

static uint8        g_reg[LINK_REG_BYTES];
static uint8        g_regA[LINK_REG_BYTES];
static uint8        g_regB[LINK_REG_BYTES];
static uint8        g_seg[SPIHAL_MAX_DATA];

static Link_State   g_state = LINK_DOWN;
static uint32       g_aliveSeen;
static uint8        g_aliveValid;
static uint32       g_aliveMs;
static uint32       g_nextPollMs;
static Link_Stats   g_stats;
static Link_Health  g_health;           /* bench watch expression, see link_updateHealth */

static uint8 link_txNext(uint8 idx)
{
    return (uint8)((idx + 1u) % LINK_TXQ_DEPTH);
}

/* ---- register file --------------------------------------------------------- */

static uint32 link_reg(const uint8 *snap, uint8 ofs)
{
    return SF_getU32(&snap[ofs]);
}

static boolean link_readRegs(uint8 *snap)
{
    /* One transaction for the whole 24 byte window. Espressif's own master
     * driver only ever reads four bytes per RDBUF (essl_spi.c:essl_spi_rdbuf),
     * so a multi word block read is on the G1 measurement list; if the slave
     * answers that with garbage this function is where it shows up. */
    return (boolean)(SPIHAL_transaction(SPIHD_CMD_RDBUF, LINK_REG_READY, NULL_PTR,
                                        snap, LINK_REG_BYTES, TRUE) == SPIHAL_OK);
}

static boolean link_regsStable(const uint8 *a, const uint8 *b)
{
    uint8 i;

    for (i = 0u; i < (sizeof(g_stableRegOfs) / sizeof(g_stableRegOfs[0])); i++)
    {
        if (memcmp(&a[g_stableRegOfs[i]], &b[g_stableRegOfs[i]], 4u) != 0)
        {
            return FALSE;
        }
    }
    return TRUE;
}

/* Double read until the decision fields agree, because the slave may rewrite a
 * register while SPI is clocking it out (22 SS2 E5). The second pass is the one
 * that is used: it is the fresher of the two. */
static boolean link_snapshot(void)
{
    uint8 attempt;

    for (attempt = 0u; attempt < LINK_REG_RETRY_MAX; attempt++)
    {
        if (attempt > 0u)
        {
            g_stats.regRetries++;
        }
        if (!link_readRegs(g_regA))
        {
            g_stats.spiErrors++;
            continue;
        }
        if (!link_readRegs(g_regB))
        {
            g_stats.spiErrors++;
            continue;
        }
        if (link_regsStable(g_regA, g_regB))
        {
            memcpy(g_reg, g_regB, sizeof(g_reg));
            return TRUE;
        }
    }
    g_stats.regUnstable++;
    return FALSE;
}

/* ---- receive path ---------------------------------------------------------- */

static void link_dispatch(const SF_Frame *frame)
{
    XcoreCmdMsg msg;

    if (frame->type != SF_TYPE_CMD)
    {
        /* ACK / HBT / OTA / DBG have no consumer in this build yet. Counted, not
         * silently dropped, so a C6 that starts sending them shows up in health. */
        g_stats.unhandledType++;
        return;
    }

    /* 22 SS5.5 as implemented: payload[0] is the command byte of the existing
     * demo command set, payload[1..] are its data bytes unchanged. That keeps the
     * command semantics inherited from the UART era byte for byte. */
    if ((frame->len < 1u) || (frame->len > LINK_CMD_DATA_MAX))
    {
        /* No truncation: a command wider than the CPU0 mailbox is a protocol
         * mismatch and must be visible, not quietly shortened. */
        g_stats.cmdOversize++;
        return;
    }

    if (frame->payload[0] == PROTO_CMD_EMERGENCY_STOP)
    {
        /* Bypass the queue depth and the CPU0 control period: the e-stop is
         * latched by CPU0 on the next read of the shared block either way. */
        XCORE_estopRequest();
    }

    msg.cmd = frame->payload[0];
    msg.len = (uint8)(frame->len - 1u);
    if (msg.len > 0u)
    {
        memcpy(msg.data, &frame->payload[1], msg.len);
    }

    if (XCORE_cmdPush(&msg) == FALSE)
    {
        g_stats.cmdRejectedQueue++;
        XCORE_logln("LINK cmdq full");
        return;
    }
    g_stats.cmdForwarded++;
}

static void link_feedSegment(const uint8 *data, uint16 len, uint32 nowMs)
{
    SF_Frame frame;
    uint16   i;
    SF_Event event;

    for (i = 0u; i < len; i++)
    {
        event = SF_parserFeed(&g_rxParser, data[i], nowMs, &frame);
        switch (event)
        {
            case SF_EV_FRAME:
                g_stats.rxFrames++;
                link_dispatch(&frame);
                break;
            case SF_EV_CRC_ERR:
                g_stats.crcErrors++;
                break;
            case SF_EV_SEQ_ERR:
                g_stats.seqErrors++;
                break;
            default:
                break;      /* resynchronisation is the codec's job */
        }
    }
}

static void link_drainRx(uint32 pending)
{
    uint32 attempted = 0u;

    if (pending > LINK_RX_BURST_MAX)
    {
        g_stats.rdBurstClamped++;
        pending = LINK_RX_BURST_MAX;
    }

    while (pending > 0u)
    {
        uint16  n = (pending > SPIHAL_MAX_DATA) ? (uint16)SPIHAL_MAX_DATA
                                                : (uint16)pending;
        n = (uint16)((n + (SF_SEG_ALIGN - 1u)) & ~(uint16)(SF_SEG_ALIGN - 1u));
        if (n > SPIHAL_MAX_DATA)
        {
            n = SPIHAL_MAX_DATA;
        }

        attempted++;
        if (SPIHAL_transaction(SPIHD_CMD_RDDMA, 0u, NULL_PTR, g_seg, n, TRUE) != SPIHAL_OK)
        {
            g_stats.spiErrors++;
            break;
        }
        g_stats.rdSegments++;
        link_feedSegment(g_seg, n, STIME_nowMs());

        if ((uint32)n > pending)
        {
            /* Rounding up past the slave's own count: the extra pad bytes belong
             * to the last frame, so nothing is lost by stopping here. */
            break;
        }
        pending -= (uint32)n;
    }

    if (attempted > 0u)
    {
        /* A RDDMA burst is only complete for the slave once it sees the INT0
         * command: spi_slave_hd counts segments and frees the buffer on that
         * event (essl_spi.c:essl_spi_rddma() then essl_spi_rddma_done()).
         * Skipping it would wedge the slave's TX slot. */
        if (SPIHAL_transaction(SPIHD_CMD_INT0, 0u, NULL_PTR, NULL_PTR, 0u, FALSE) != SPIHAL_OK)
        {
            g_stats.spiErrors++;
        }
    }
}

/* ---- transmit path --------------------------------------------------------- */

static void link_pumpTx(uint32 room)
{
    uint16 used = 0u;
    int16_t size;

    if ((g_txHead == g_txTail) || (room < SF_OVERHEAD))
    {
        return;
    }

    while (g_txHead != g_txTail)
    {
        const Link_TxSlot *slot = &g_txq[g_txHead];

        if ((uint32)SF_wireSize(slot->len) > (uint32)(SPIHAL_MAX_DATA - used))
        {
            break;      /* segment full, the rest waits for the next cycle */
        }

        size = SF_build(slot->type, g_txSeq, 0u, slot->cid, slot->payload,
                        slot->len, &g_seg[used], (uint16)(SPIHAL_MAX_DATA - used));
        if (size < 0)
        {
            /* Unreachable: the fit was just checked with the codec's own size
             * calculation. Guarded anyway so a codec regression drops one slot
             * instead of corrupting the segment length. */
            g_stats.txDropped++;
            g_txHead = link_txNext(g_txHead);
            continue;
        }

        used = (uint16)(used + (uint16)size);
        g_txSeq = (uint8)(g_txSeq + 1u);
        g_txHead = link_txNext(g_txHead);

        if ((uint32)used >= room)
        {
            break;      /* slave says it has no more RX room */
        }
    }

    if (used == 0u)
    {
        return;
    }

    if (SPIHAL_transaction(SPIHD_CMD_WRDMA, 0u, g_seg, NULL_PTR, used, FALSE) != SPIHAL_OK)
    {
        g_stats.spiErrors++;
        return;
    }
    g_stats.wrSegments++;
    g_stats.txFrames++;

    /* Closing transaction of a WRDMA burst (essl_spi.c:essl_spi_wrdma_done()):
     * without WR_END the slave never marks the segment as delivered. */
    if (SPIHAL_transaction(SPIHD_CMD_WR_END, 0u, NULL_PTR, NULL_PTR, 0u, FALSE) != SPIHAL_OK)
    {
        g_stats.spiErrors++;
    }
}

/* ---- state / liveness ------------------------------------------------------ */

static void link_setState(Link_State state)
{
    if (state == g_state)
    {
        return;
    }
    g_state = state;

    if (state == LINK_LOST)
    {
        g_stats.linkLost++;
        /* 22 SS5.4: a dead slave must not leave the drivetrain running. The stop
         * is a latched request CPU0 acts on; losing the link never means losing
         * control. */
        XCORE_estopRequest();
        XCORE_logln("LINK lost");
    }
    else if (state == LINK_READY)
    {
        XCORE_logln("LINK up");
    }
    else
    {
        XCORE_logln("LINK down (no SF_READY)");
    }
}

static void link_updateLiveness(uint32 nowMs)
{
    uint32 alive = link_reg(g_reg, LINK_REG_ALIVE);

    if (!g_aliveValid || (alive != g_aliveSeen))
    {
        g_aliveSeen  = alive;
        g_aliveValid = 1u;
        g_aliveMs    = nowMs;
        return;
    }

    if ((uint32)(nowMs - g_aliveMs) >= LINK_ALIVE_TIMEOUT_MS)
    {
        link_setState(LINK_LOST);
    }
    else
    {
        link_setState(LINK_READY);
    }
}

/* ---- public ---------------------------------------------------------------- */

void LINK_init(SpiHal_ClockTier tier)
{
    memset(&g_stats, 0, sizeof(g_stats));
    memset((void *)&g_health, 0, sizeof(g_health));
    memset(g_txq, 0, sizeof(g_txq));
    g_txHead = 0u;
    g_txTail = 0u;
    g_txSeq  = 0u;

    SF_parserInit(&g_rxParser);
    g_aliveValid = 0u;
    g_aliveSeen  = 0u;
    g_aliveMs    = 0u;
    g_state      = LINK_DOWN;

    SPIHAL_init(tier);
    g_nextPollMs = STIME_nowMs();
}

static void link_cycle(void)
{
    uint32 nowMs;
    uint32 ready;
    uint32 pending;
    uint32 room;

    nowMs = STIME_nowMs();

    /* The slave signals queued data on P23.0, but that pin can only be sampled,
     * not made to interrupt (22 SS4.1 write back). Either way the poll runs at
     * least every LINK_KEEPALIVE_MS, which bounds the command latency at the
     * 2.3 ms figure of 22 SS6 even if the IRQ line or its pull-up is missing. */
    if ((SPIHAL_irqAsserted() == FALSE) && ((sint32)(nowMs - g_nextPollMs) < 0))
    {
        return;
    }
    g_nextPollMs = nowMs + LINK_KEEPALIVE_MS;

    if (!link_snapshot())
    {
        /* Registers are not trustworthy: treat as no link. LOST is only reported
         * from a link that was READY, so booting without a flashed slave stays a
         * quiet LINK_DOWN instead of a stop request. */
        if (g_state == LINK_READY)
        {
            link_setState(LINK_LOST);
        }
        return;
    }

    ready = link_reg(g_reg, LINK_REG_READY);
    if (ready != LINK_READY_MAGIC)
    {
        g_aliveValid = 0u;
        link_setState(LINK_DOWN);
        return;
    }

    pending = link_reg(g_reg, LINK_REG_TX_PENDING);
    room    = link_reg(g_reg, LINK_REG_RX_ROOM);

    if (pending > 0u)
    {
        link_drainRx(pending);
    }
    link_pumpTx(room);

    /* A segment can end mid frame; if no continuation arrives within the codec's
     * residual window, discard it and resynchronise (22 SS5.4). */
    (void)SF_parserTick(&g_rxParser, STIME_nowMs());

    link_updateLiveness(STIME_nowMs());
}

boolean LINK_send(uint8 type, uint8 cid, const uint8 *payload, uint8 len)
{
    Link_TxSlot *slot;
    uint8        next;

    if ((len > SF_MAX_PAYLOAD) || ((len > 0u) && (payload == NULL_PTR)))
    {
        return FALSE;
    }

    next = link_txNext(g_txTail);
    if (next == g_txHead)
    {
        g_stats.txQueueFull++;
        return FALSE;      /* queue full: backpressure to the caller */
    }

    slot = &g_txq[g_txTail];
    slot->type = type;
    slot->cid  = cid;
    slot->len  = len;
    if (len > 0u)
    {
        memcpy(slot->payload, payload, len);
    }
    g_txTail = next;
    return TRUE;
}

boolean LINK_isUp(void)
{
    return (boolean)(g_state == LINK_READY);
}

/* Gates G1/G5 are read on the bench, and CPU2 owns no printable channel in this
 * build (ASCLIN0 is CPU0's, ASCLIN1's pins are the slave's console), so the
 * counters are published as a watch expression instead of a log line. */
static void link_updateHealth(uint32 nowMs)
{
    SPIHAL_getStats(&g_health.spi);

    g_health.state        = g_state;
    g_health.sinceAliveMs = g_aliveValid ? (uint32)(nowMs - g_aliveMs) : 0u;
    g_health.txPending    = link_reg(g_reg, LINK_REG_TX_PENDING);
    g_health.rxRoom       = link_reg(g_reg, LINK_REG_RX_ROOM);
    g_health.slaveErrStat = link_reg(g_reg, LINK_REG_ERRSTAT);
    g_health.clock        = SPIHAL_getClock();
    g_health.clockHz      = SPIHAL_actualClockHz();
    g_health.stats        = g_stats;
    g_health.frameStats   = g_rxParser.stats;
}

void LINK_main(void)
{
    link_cycle();
    link_updateHealth(STIME_nowMs());
}

void LINK_getHealth(Link_Health *health)
{
    if (health != NULL_PTR)
    {
        *health = g_health;
    }
}

