/*
 * link.c - SF-over-SPI link pump, TC275 (CPU2) master side
 *
 * Implements the cycle of doc/20-design/22-link-spi-design.md SS4.2 in the order
 * the document fixes it: register snapshot, then read (higher priority), then
 * write, then liveness. Owner core: CPU2, bare metal, superloop context only.
 */
#include "com/link.h"

#include "com/ota_app.h"
#include "mw/ota/ota_rx.h"
#include "mw/xcore/xcore.h"
#include "bsp/stime.h"

#include <string.h>

/* TX_PENDING above this cannot be a real buffer claim (the slave's whole buffer
 * is smaller), so it is treated as a bad register read and the drain loop is
 * shortened. 640 B is two and a half maximum segments. */
#define LINK_RX_BURST_MAX     640u

/* Rate limit of the "LINK cmdq full" console line: first rejection logs at
 * once, then at most one line per second carries the suppressed count. The
 * counter in Link_Stats stays one-for-one either way - the line is for the
 * human, the counter is for the tripwire. */
#define LINK_CMDQ_FULL_LOG_MS 5000u

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
static uint8        g_seg[SPIHAL_MAX_DATA];

static Link_State   g_state = LINK_DOWN;
static uint32       g_aliveSeen;
static uint8        g_aliveValid;
static uint32       g_aliveMs;
static uint32       g_nextPollMs;
static uint32       g_telSeq;            /* E2E counter of the telemetry stream */
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

/* One RDBUF read per cycle, used defensively instead of being trusted blindly.
 *
 * The slave rewrites its register file whenever a segment completes or its
 * 10 ms ALIVE tick fires, so a value read here can legitimately be torn or a
 * few hundred microseconds stale. The previous double-read-and-compare turned
 * exactly that into regUnstable, and regUnstable into LINK_LOST + e-stop -
 * with a busy link the compared fields (TX_PENDING, RX_ROOM) change all the
 * time, so the faster the link ran, the more often the master declared it
 * lost: the "works idle, dies under load" field failure. The consumers now
 * tolerate what a single read can produce:
 *   READY      static while the slave is up; a wrong value reads as LINK_DOWN
 *              and the next good read recovers through the READY edge
 *   TX_PENDING clamped to LINK_RX_BURST_MAX in link_drainRx (pre-existing)
 *   RX_ROOM    clamped to LINK_RX_ROOM_MAX in link_cycle
 *   ALIVE      meaningful even when torn - and it is the only truth source
 *              for liveness (LINK_ALIVE_TIMEOUT_MS), never the snapshot */
static boolean link_snapshot(void)
{
    g_stats.polls++;
    if (!link_readRegs(g_reg))
    {
        g_stats.spiErrors++;
        return FALSE;
    }
    return TRUE;
}

/* ---- receive path ---------------------------------------------------------- */

/* CID first, payload byte second.
 *
 * c6_link puts the v2 command byte in payload[0] for the DRV/DIAG/DPT families,
 * but for CFG and PAIR it copies the v2 payload verbatim and lets the CID carry
 * the identity of the message (esp32c6_car components/c6_link/link.c:v2_to_sf). A
 * dispatch that assumed "payload[0] is always a command" would therefore execute
 * a configuration key or the first byte of a pairing token as if it were a drive
 * command - the one mistake on this link that can move the car.
 *
 * Of the three command-bearing channels, this build consumes one:
 *   SF_CID_DRIVE  {u8 op, i16 v, i16 w}      op is a v2 code, and doc 21 SS6.2
 *                                            keeps 0x01..0x32 identical to the
 *                                            demo command set, so op crosses
 *                                            over unchanged. v/w are only filled
 *                                            for 0x10 SET_SPEED (two +-100
 *                                            targets, doc 11 SS7: +100 is full
 *                                            speed, so the i16 narrows to the
 *                                            demo's sint8 with nothing lost) and
 *                                            for 0x50 DRIVE (v mm/s, omega deg/s
 *                                            per second), which link_driveSpeeds
 *                                            mixes into two wheel percents and
 *                                            forwards as SET_SPEED - CPU0's
 *                                            ROBOT_cmdSetSpeeds already accepts
 *                                            an arbitrary differential. The mix
 *                                            normalises at 600 mm/s / 300 deg/s
 *                                            (the joystick's full deflection,
 *                                            esp32c6_car assets_src/app.js) instead of
 *                                            waiting for the SS11 kinematics in
 *                                            physical units; the closed loop
 *                                            still lives on CPU1 (motor targets
 *                                            from CPU0 are percent x 10).
 *   SF_CID_DIAG   {u8 op, ...}               op 0x53 / 0x42; CPU0 consumes
 *                                            0x53 sub 0x24 (version request
 *                                            -> immediate EVT 0x24/0x25)
 *   SF_CID_DPT    {u8 op, ...}               op 0x70..0x79, the bench
 *                                            calibration/DFlash-record family
 *                                            (doc 34 SS9); results and record
 *                                            echoes come back as TYPE_EVT CIDs
 *                                            0x22 / 0x23 on the event outbox
 * The last two go to CPU0 as op + remaining bytes, which is what the demo path
 * did over UART; CPU0's switch ignores codes it does not know, and the counters
 * here say what was seen.
 */
#define LINK_DRV_PAYLOAD_LEN  5u   /* {u8 op, i16 v, i16 w}, 22 SS5.5 */

/* v2 code for the joystick stream (doc 21 SS6.2). It is not in protocol.h,
 * which is the demo's UART command table and stops at 0x32. */
#define LINK_OP_DRIVE           0x50u

/* Full-deflection anchors of the joystick mix: 600 mm/s and 300 deg/s both
 * normalise to 100 % (esp32c6_car assets_src/app.js joyMove). Sign convention:
 * w > 0 is CCW (left turn), so the right wheel gets the +w share. */
#define LINK_DRIVE_V_FULL       600
#define LINK_DRIVE_W_FULL       300

static sint8 link_narrowPercent(uint16 raw)
{
    sint16 value = (sint16)raw;

    if (value > 100)
    {
        value = 100;
    }
    else if (value < -100)
    {
        value = -100;
    }
    return (sint8)value;
}

/* 0x50 DRIVE {v mm/s, w deg/s} -> {left,right} percent, the payload shape of
 * SET_SPEED that CPU0 already executes. Arcade mix, clamped to +-100 per side
 * so a full-speed-full-turn command pivots instead of wrapping. */
static void link_driveSpeeds(sint16 v, sint16 w, uint8 *out)
{
    sint32 lPct;
    sint32 rPct;

    lPct = ((sint32)v * 100 / LINK_DRIVE_V_FULL) - ((sint32)w * 100 / LINK_DRIVE_W_FULL);
    rPct = ((sint32)v * 100 / LINK_DRIVE_V_FULL) + ((sint32)w * 100 / LINK_DRIVE_W_FULL);

    if (lPct > 100)
    {
        lPct = 100;
    }
    if (lPct < -100)
    {
        lPct = -100;
    }
    if (rPct > 100)
    {
        rPct = 100;
    }
    if (rPct < -100)
    {
        rPct = -100;
    }

    out[0] = (uint8)(sint8)lPct;
    out[1] = (uint8)(sint8)rPct;
}

/* Queue one command for CPU0, with the e-stop bypassed ahead of the queue.
 *
 * SET_SPEED rides XCORE_cmdPushLatest: it is the carrier of the 30 Hz
 * joystick/heartbeat stream, where each frame supersedes the previous one, so
 * a burst of stale copies (the C6 packs its whole backlog into one 512 B
 * segment after any pump stall) collapses into the latest position instead of
 * overflowing the queue. Every other command keeps strict FIFO - for one-shot
 * and safety commands the ordering is part of their meaning. */
static void link_forward(uint8 cmd, const uint8 *data, uint8 len)
{
    XcoreCmdMsg msg;
    boolean     queued;

    if (len > LINK_CMD_DATA_MAX)
    {
        /* No truncation: a command wider than the CPU0 mailbox is a protocol
         * mismatch and must be visible, not quietly shortened. */
        g_stats.cmdOversize++;
        return;
    }

    if (cmd == PROTO_CMD_EMERGENCY_STOP)
    {
        /* Bypass the queue depth and the CPU0 control period: the e-stop is
         * latched by CPU0 on the next read of the shared block either way. */
        XCORE_estopRequest();
    }

    msg.cmd = cmd;
    msg.len = len;
    if (len > 0u)
    {
        memcpy(msg.data, data, len);
    }

    queued = (cmd == PROTO_CMD_SET_SPEED) ? XCORE_cmdPushLatest(&msg)
                                          : XCORE_cmdPush(&msg);
    if (queued == FALSE)
    {
        /* Rejection is still counted one-for-one (the counter is the tripwire
         * the LINKDBG line carries), but the log line is rate limited: with
         * coalescing above this only fires on genuinely diverse traffic, and
         * one line per rejected command used to be its own congestion
         * amplifier - CPU0 burned a full ring drain printing the burst while
         * the queue it was complaining about stayed full. */
        static uint32 s_lastLogMs = 0u;
        static uint32 s_lastCount = 0u;
        static boolean s_everLogged = FALSE;
        uint32 nowMs = STIME_nowMs();

        g_stats.cmdRejectedQueue++;
        if ((s_everLogged == FALSE) ||
            ((uint32)(nowMs - s_lastLogMs) >= LINK_CMDQ_FULL_LOG_MS))
        {
            uint32 suppressed = g_stats.cmdRejectedQueue - s_lastCount;
            XCORE_LOG_FIELDS("[LINK_COMMAND]", XL_U("uptime_ms", nowMs),
                XL_U("rejected_since_last_log", suppressed),
                XL_U("rejected_total", g_stats.cmdRejectedQueue));
            s_everLogged = TRUE;
            s_lastLogMs  = nowMs;
            s_lastCount  = g_stats.cmdRejectedQueue;
        }
        return;
    }
    g_stats.cmdForwarded++;
}

static void link_dispatch(const SF_Frame *frame)
{
    uint8 op;
    uint8 speed[2];

    if ((frame->type == SF_TYPE_OTA_DATA) || (frame->type == SF_TYPE_OTA_CTRL))
    {
        /* OTA receiver (doc 24 SS5.3): BEGIN/CHUNK/ABORT/SWAP drive the
         * whole transfer, including the flash writes, right here in the CPU2
         * link context. ACK/STATUS replies ride LINK_send; a SWAP resets the
         * MCU into the SBL. Synchronous: the payload is consumed before
         * this call returns (no copy needed). */
        OTARX_frame(frame->type, frame->cid, frame->payload, frame->len);
        return;
    }

    if (frame->type != SF_TYPE_CMD)
    {
        /* ACK / HBT / DBG have no consumer in this build yet. Counted, not
         * silently dropped, so a C6 that starts sending them shows up in health. */
        g_stats.unhandledType++;
        return;
    }

    if ((frame->cid != SF_CID_DRIVE) && (frame->cid != SF_CID_DIAG) &&
        (frame->cid != SF_CID_DPT))
    {
        g_stats.cmdUnsupportedCid++;
        return;
    }

    if (frame->len < 1u)
    {
        g_stats.cmdBadLen++;
        return;
    }

    op = frame->payload[0];

    if (frame->cid == SF_CID_DRIVE)
    {
        if (frame->len != LINK_DRV_PAYLOAD_LEN)
        {
            /* The shape is fixed by the sender; anything else is a version skew
             * and must not be decoded by offset. */
            g_stats.cmdBadLen++;
            return;
        }

        if (op == PROTO_CMD_GET_STATUS)
        {
            /* Answered out of the published status block by the next telemetry
             * frame, at most one period away; CPU0 has no use for it. */
            return;
        }

        if (op == LINK_OP_DRIVE)
        {
            uint8 speeds[2];

            link_driveSpeeds((sint16)SF_getU16(&frame->payload[1]),
                             (sint16)SF_getU16(&frame->payload[3]), speeds);
            link_forward(PROTO_CMD_SET_SPEED, speeds, 2u);
            return;
        }

        if (op == PROTO_CMD_SET_SPEED)
        {
            speed[0] = (uint8)link_narrowPercent(SF_getU16(&frame->payload[1]));
            speed[1] = (uint8)link_narrowPercent(SF_getU16(&frame->payload[3]));
            link_forward(op, speed, 2u);
            return;
        }

        /* Discrete command: v/w are zero for every other op, so there is nothing
         * to carry across the cores. */
        link_forward(op, NULL_PTR, 0u);
        return;
    }

    link_forward(op, &frame->payload[1], (uint8)(frame->len - 1u));
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
    uint16 frames = 0u;
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
        frames++;
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
    /* Counted once the segment is on the wire, and per frame rather than per
     * segment: several frames share one transaction, so counting segments would
     * understate the traffic and hide a queue that never drains. */
    g_stats.wrSegments++;
    g_stats.txFrames += frames;

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
        /* Fresh READY edge (boot, or recovery from DOWN/LOST): the slave's TX
         * SEQ restarted with it, so drop our RX window and re-lock on its next
         * frame. Our own TX SEQ deliberately keeps running - the slave re-locks
         * onto the first frame it sees after a restart (both codecs drop their
         * window after SF_SEQ_RELOCK_RUN consecutive rejects). The former GEN
         * RESET_LINK handshake did not survive contact with that reality: its
         * no-reply retry path could hold the pump hostage forever (resync
         * livelock: state LINK_READY, but no RDDMA/WRDMA and a telemetry queue
         * that overflows silently - the other half of the "cannot connect"
         * field failures). It survives as a bench/diag entry point (LINK_gen). */
        SF_parserInit(&g_rxParser);
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
    g_aliveValid    = 0u;
    g_aliveSeen     = 0u;
    g_aliveMs       = 0u;
    g_state         = LINK_DOWN;

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
     * 2.3 ms figure of 22 SS6 even when the IRQ line carries nothing useful -
     * with jumper wires and only P23.0's weak internal pull-up there is no
     * external resistor to rely on, so a stuck or floating level is a normal
     * state to design for, not a fault case (doc 23 SS9.3, SDD SS18 C14). */
    if ((SPIHAL_irqAsserted() == FALSE) && ((sint32)(nowMs - g_nextPollMs) < 0))
    {
        return;
    }
    g_nextPollMs = nowMs + LINK_KEEPALIVE_MS;

    if (!link_snapshot())
    {
        /* A transaction that did not complete cleanly says nothing about the
         * slave: the wire glitched or the driver re-armed itself. Skip the
         * data movement this cycle; only LINK_ALIVE_TIMEOUT_MS declares the
         * slave gone (and that check runs below, from a good snapshot). */
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
    if (room > LINK_RX_ROOM_MAX)
    {
        /* Torn or stale read: the slave owns two 512 B RX DMA buffers and can
         * never honestly advertise more room than that. Clamping turns a
         * garbage register into one slowed-down cycle instead of an oversized
         * write burst into a slave that is not expecting it. */
        room = LINK_RX_ROOM_MAX;
    }

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

/* ---- telemetry producer ---------------------------------------------------- */

boolean LINK_sendTelemetry(const SF_Telemetry *tel)
{
    SF_Telemetry timed;
    uint8        buf[SF_TELEMETRY_LEN];

    if (tel == NULL_PTR)
    {
        return FALSE;
    }

    /* The link owns the E2E counter: it is the sequence of what actually went on
     * the wire, so it advances once per accepted frame rather than once per call
     * that tried. Everything else belongs to the producer. */
    timed = *tel;
    g_telSeq++;
    timed.seq = g_telSeq;

    if (SF_telemetryEncode(&timed, buf, (uint16)sizeof(buf)) < 0)
    {
        return FALSE;
    }

    return LINK_send(SF_TYPE_TEL, SF_CID_TELEMETRY, buf, (uint8)SF_TELEMETRY_LEN);
}

/* ---- GEN register: register level control of the slave --------------------- */

/* Write the GEN slot and wait for the receipt of this command byte. *receipt
 * receives LINK_REG_CMDRSP whenever the slave answered; FALSE means it never
 * did, which on a healthy slave means the SPI path itself is at fault. */
static boolean link_genWriteAndWait(uint8 cmd, uint32 payload, uint32 timeoutMs,
                                    uint32 *receipt)
{
    uint8  word[4];
    uint32 deadline;
    uint32 rsp;

    word[0] = cmd;
    word[1] = (uint8)(payload & 0xFFu);
    word[2] = (uint8)((payload >> 8) & 0xFFu);
    word[3] = (uint8)((payload >> 16) & 0xFFu);

    g_stats.genWrites++;
    if (SPIHAL_transaction(SPIHD_CMD_WRBUF, LINK_REG_GEN, word, NULL_PTR, 4u, FALSE) != SPIHAL_OK)
    {
        g_stats.spiErrors++;
        return FALSE;
    }

    deadline = STIME_nowMs() + timeoutMs;
    for (;;)
    {
        if (!link_snapshot())
        {
            return FALSE;              /* counted inside link_snapshot */
        }
        rsp = link_reg(g_reg, LINK_REG_CMDRSP);
        if ((uint8)rsp == cmd)
        {
            *receipt = rsp;
            return TRUE;
        }
        if ((sint32)(STIME_nowMs() - deadline) >= 0)
        {
            g_stats.genNoAck++;
            return FALSE;
        }
    }
}

/* The slave's receipt is sticky, so sending the same command twice would be
 * answered with the previous receipt and look like an instant success. A NOP
 * first clears the slot: the slave has no NOP case, so it answers
 * {0, UNKNOWN}, which no real command can be confused with. */
Link_GenResult LINK_gen(uint8 cmd, uint32 payload, uint32 timeoutMs)
{
    uint32 receipt;

    if (payload > 0x00FFFFFFu)
    {
        /* The slot carries 24 payload bits (link_handle_gen reads four bytes:
         * one command plus three). Refuse instead of silently masking. */
        return LINK_GEN_ERR_RANGE;
    }

    if ((cmd != LINK_GEN_NOP) &&
        (link_genWriteAndWait(LINK_GEN_NOP, 0u, timeoutMs, &receipt) == FALSE))
    {
        return LINK_GEN_ERR_NOACK;
    }

    if (link_genWriteAndWait(cmd, payload, timeoutMs, &receipt) == FALSE)
    {
        return LINK_GEN_ERR_NOACK;
    }

    if (((receipt >> 8) & 0xFFu) != LINK_GEN_RESULT_OK)
    {
        return LINK_GEN_ERR_REJECT;
    }
    return LINK_GEN_OK;
}

SpiHal_Status LINK_setClock(SpiHal_ClockTier tier)
{
    SpiHal_Status status;
    uint32        hz;

    status = SPIHAL_setClock(tier);
    if (status != SPIHAL_OK)
    {
        return status;
    }

    /* The slave stores the figure for its own diagnostics only (link_handle_gen
     * multiplies it back into health.clock_hz); the wire clock is the master's
     * property. So a refused mirror is reported through stats.genNoAck and does
     * not undo the local switch. */
    if (LINK_isUp())
    {
        hz = SPIHAL_clockHz(tier);
        (void)LINK_gen(LINK_GEN_CLOCK_SET, hz / 1000000u, LINK_GEN_TIMEOUT_MS);
    }
    return status;
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

/* Snapshot only: state changes are immediate, repeated faults are rate limited.
 * Slave registers are meaningful only after the READY magic is observed. */
void LINK_diagPrint(void)
{
    static uint32 prevState = 0xFFFFFFFFu, prevClock = 0u, prevReady = 0u;
    static uint32 prevFaults = 0u, prevSlave = 0u, lastMs = 0u;
    static boolean initialized = FALSE;
    uint32 now = STIME_nowMs();
    uint32 state = (uint32)g_health.state;
    uint32 ready = link_reg(g_reg, LINK_REG_READY);
    boolean valid = (boolean)(ready == LINK_READY_MAGIC);
    uint32 slave = valid ? g_health.slaveErrStat : 0u;
    uint32 faults = g_health.spi.timeouts + g_health.spi.hwErrors
                  + g_health.stats.spiErrors + g_health.stats.crcErrors
                  + g_health.stats.seqErrors + g_health.stats.txQueueFull
                  + g_health.stats.cmdRejectedQueue;
    boolean transition = (boolean)(!initialized || state != prevState
                         || ready != prevReady || g_health.clockHz != prevClock);
    boolean faultChange = (boolean)(faults != prevFaults || slave != prevSlave);
    if (!transition && (uint32)(now - lastMs) < 30000u
        && !(faultChange && ((prevFaults == 0u && prevSlave == 0u)
                             || (uint32)(now - lastMs) >= 5000u))) return;
    initialized = TRUE; lastMs = now;
    prevState = state; prevClock = g_health.clockHz; prevReady = ready;
    prevFaults = faults; prevSlave = slave;
    XCORE_LOG_FIELDS("[LINK]", XL_U("uptime_ms", now),
        XL_S("state", state == LINK_READY ? "ready" : state == LINK_LOST ? "lost" : "down"),
        XL_U("registers_valid", valid), XL_U("spi_hz", g_health.clockHz),
        XL_U("irq_asserted", SPIHAL_irqAsserted()), XL_H("ready_reg", ready));
    if (valid)
    {
        XCORE_LOG_FIELDS("[LINK_SLAVE]", XL_U("uptime_ms", now),
            XL_U("tx_pending_bytes", g_health.txPending), XL_U("rx_room_bytes", g_health.rxRoom),
            XL_U("alive_age_ms", g_health.sinceAliveMs), XL_H("error_bits", slave));
    }
    else XCORE_LOG_FIELDS("[LINK_SLAVE]", XL_U("uptime_ms", now), XL_S("data", "unknown"));
    XCORE_LOG_FIELDS("[LINK_ERRORS]", XL_U("uptime_ms", now),
        XL_U("spi_timeout_total", g_health.spi.timeouts), XL_U("spi_hw_error_total", g_health.spi.hwErrors),
        XL_U("spi_error_total", g_health.stats.spiErrors), XL_U("crc_error_total", g_health.stats.crcErrors),
        XL_U("sequence_error_total", g_health.stats.seqErrors),
        XL_U("tx_queue_full_total", g_health.stats.txQueueFull),
        XL_U("cmd_rejected_total", g_health.stats.cmdRejectedQueue));
    XCORE_LOG_FIELDS("[LINK_TRAFFIC]", XL_U("uptime_ms", now),
        XL_U("spi_transactions_total", g_health.spi.transactions),
        XL_U("write_segments_total", g_health.stats.wrSegments),
        XL_U("read_segments_total", g_health.stats.rdSegments),
        XL_U("tx_frames_total", g_health.stats.txFrames));
}
