/*
 * ota_rx.c - the doc 24 §5.3 receive state machine over injected ops
 */
#include "ota_rx.h"

#include <string.h>

#include "../crypto/sha512.h"
#include "crc32.h"
#include "../sf/sf_frame.h"

/* Read-back window for the post-flash digest check: SHA-512 over the written
 * image straight out of the slot. 4 KB keeps the stack flat on a core with a
 * 2 KB-ish spare budget; total time is dominated by the hash, not the loop. */
#define OTARX_VERIFY_WIN    4096u

typedef struct
{
    const OtaRxOps *ops;

    OtaRxState state;
    uint8_t     target;          /* OTA_SLOT_NONE while idle                 */
    uint32_t    totalLen;        /* whole bundle bytes (header + image)      */
    uint32_t    wantCrc;         /* CRC-32 of the whole bundle               */
    uint32_t    runCrc;          /* CRC-32 over received bundle bytes        */
    uint32_t    rxTotal;         /* received bundle bytes                    */
    uint16_t    nextIdx;         /* next expected chunk index                */
    uint16_t    lastIdx;         /* last accepted chunk index (0xFFFF none)  */
    uint8_t     lastPct;
    uint32_t    frames;
    TcfwCtx     tcfw;
} OtaRxCtx;

static OtaRxCtx s_rx;

/* ---- helpers ---------------------------------------------------------------- */

static void rx_sendStatus(uint8_t state, uint8_t pct)
{
    uint8_t p[2];

    p[0] = state;
    p[1] = pct;
    if (s_rx.ops->send != 0)
    {
        s_rx.ops->send(SF_TYPE_OTA_CTRL, SF_CID_OTA_STATUS, p, 2u);
    }
}

static void rx_sendAck(uint16_t idx, uint8_t result)
{
    uint8_t p[3];

    p[0] = (uint8_t)(idx & 0xFFu);
    p[1] = (uint8_t)(idx >> 8);
    p[2] = result;
    if (s_rx.ops->send != 0)
    {
        s_rx.ops->send(SF_TYPE_OTA_CTRL, SF_CID_OTA_ACK, p, 3u);
    }
}

static void rx_failSession(uint8_t slot)
{
    if (s_rx.ops->metaAborted != 0)
    {
        s_rx.ops->metaAborted(slot);
    }
    s_rx.state  = OTARX_IDLE;
    s_rx.target = OTA_SLOT_NONE;
    rx_sendStatus(OTARX_STATUS_FAILED, 0u);
}

/* Sink for TCFW payload bytes: straight into the slot staging page. */
static int rx_payloadSink(void *arg, uint32_t off, const uint8_t *d, uint32_t n)
{
    (void)arg;
    return (s_rx.ops->writeApp(s_rx.target, off, d, n) == 1u) ? 0 : 1;
}

/* Re-hash the written image out of flash and compare against the signed
 * digest from the bundle header. 1 = ok. */
static uint8_t rx_verifyFlashed(void)
{
    uint8_t  win[OTARX_VERIFY_WIN];
    uint8_t  digest[C6_SHA512_DIGEST_LEN];
    c6_sha512_ctx_t hash;
    uint32_t done = 0u;
    uint32_t len  = s_rx.tcfw.info.app_len;

    c6_sha512_init(&hash);
    while (done < len)
    {
        uint32_t n = len - done;

        if (n > OTARX_VERIFY_WIN)
        {
            n = OTARX_VERIFY_WIN;
        }
        if (s_rx.ops->readApp(s_rx.target, done, n, win) != 1u)
        {
            return 0u;
        }
        c6_sha512_update(&hash, win, n);
        done += n;
    }
    c6_sha512_final(&hash, digest);
    return (memcmp(digest, s_rx.tcfw.info.app_sha, 32u) == 0) ? 1u : 0u;
}

/* Everything after the last chunk: transport CRC, bundle digest, flash
 * flush + read-back, metadata, DONE status. */
static void rx_complete(void)
{
    if (crc32_final(s_rx.runCrc) != s_rx.wantCrc)
    {
        rx_failSession(s_rx.target);
        return;
    }
    if (TCFW_finish(&s_rx.tcfw) != TCFW_DONE)
    {
        rx_failSession(s_rx.target);
        return;
    }
    if (s_rx.ops->flushApp(s_rx.target, s_rx.tcfw.info.app_len) != 1u)
    {
        rx_failSession(s_rx.target);
        return;
    }
    if (rx_verifyFlashed() == 0u)
    {
        rx_failSession(s_rx.target);
        return;
    }

    s_rx.state = OTARX_DONE;
    if (s_rx.ops->metaDownloaded != 0)
    {
        s_rx.ops->metaDownloaded(s_rx.target);
    }
    rx_sendStatus(OTARX_STATUS_DONE, 100u);
}

/* ---- frame handlers ----------------------------------------------------------- */

static void rx_onBegin(const uint8_t *d, uint16_t len)
{
    uint32_t total;
    uint32_t crc;

    if (len < 8u)
    {
        rx_sendStatus(OTARX_STATUS_FAILED, 0u);
        return;
    }
    if (s_rx.state == OTARX_RECEIVING)
    {
        /* transfer already running: tell the pusher where we are */
        rx_sendStatus(OTARX_STATUS_RUNNING, OTARX_progressPct());
        return;
    }

    total = (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
            ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    crc   = (uint32_t)d[4] | ((uint32_t)d[5] << 8) |
            ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);

    if ((total <= TCFW_HDR_LEN) ||
        (total > (TCFW_HDR_LEN + OTA_SLOT_SIZE)))
    {
        rx_sendStatus(OTARX_STATUS_FAILED, 0u);
        return;
    }

    s_rx.target = (s_rx.ops->activeSlot == OTA_SLOT_B) ? OTA_SLOT_A
                                                       : OTA_SLOT_B;
    if (s_rx.ops->eraseSlot(s_rx.target) != 1u)
    {
        rx_failSession(s_rx.target);
        return;
    }

    TCFW_init(&s_rx.tcfw, s_rx.ops->pubkey);
    s_rx.totalLen = total;
    s_rx.wantCrc  = crc;
    s_rx.runCrc   = crc32_init_value();
    s_rx.rxTotal  = 0u;
    s_rx.nextIdx  = 0u;
    s_rx.lastIdx  = 0xFFFFu;
    s_rx.lastPct  = 0u;
    s_rx.state    = OTARX_RECEIVING;
    rx_sendStatus(OTARX_STATUS_RUNNING, 0u);
}

static void rx_onChunk(const uint8_t *d, uint16_t len)
{
    uint16_t idx;
    uint16_t n;
    uint8_t  pct;

    if (len < 3u)
    {
        return;                      /* not even an idx: nothing to ACK to */
    }
    idx = (uint16_t)((uint16_t)d[0] | ((uint16_t)d[1] << 8));
    n   = (uint16_t)(len - 2u);

    if ((n == 0u) || (n > OTARX_CHUNK_MAX))
    {
        rx_sendAck(idx, OTARX_ACK_BAD_LEN);
        return;
    }
    if (s_rx.state != OTARX_RECEIVING)
    {
        rx_sendAck(idx, OTARX_ACK_BUSY);
        return;
    }

    if (idx == s_rx.lastIdx)
    {
        /* retransmission of the last accepted chunk: idempotent OK */
        rx_sendAck(idx, OTARX_ACK_OK);
        return;
    }
    if (idx != s_rx.nextIdx)
    {
        /* out-of-order: the SPI link does not retransmit below us, so the
         * stream is unrecoverable - fail the session, pusher restarts */
        rx_sendAck(idx, OTARX_ACK_BAD_IDX);
        rx_failSession(s_rx.target);
        return;
    }

    /* bundle bytes are the chunk payload behind the 2-byte idx header */
    s_rx.runCrc = crc32_update(s_rx.runCrc, &d[2], n);
    {
        TcfwState st = TCFW_feed(&s_rx.tcfw, &d[2], n, 0, rx_payloadSink);

        if ((st != TCFW_NEED_HDR) && (st != TCFW_APP_DATA) &&
            (st != TCFW_DONE))
        {
            rx_sendAck(idx, (st == TCFW_ERR_IO) ? OTARX_ACK_FLASH
                                                : OTARX_ACK_VERIFY);
            rx_failSession(s_rx.target);
            return;
        }
    }

    s_rx.rxTotal += n;
    s_rx.lastIdx  = idx;
    s_rx.nextIdx  = (uint16_t)(idx + 1u);
    rx_sendAck(idx, OTARX_ACK_OK);

    pct = (uint8_t)((s_rx.rxTotal * 100u) / s_rx.totalLen);
    if (pct != s_rx.lastPct)
    {
        s_rx.lastPct = pct;
        rx_sendStatus(OTARX_STATUS_RUNNING, pct);
    }

    if ((s_rx.tcfw.state == TCFW_DONE) && (s_rx.rxTotal == s_rx.totalLen))
    {
        rx_complete();
    }
}

static void rx_onAbort(void)
{
    if (s_rx.state == OTARX_IDLE)
    {
        rx_sendStatus(OTARX_STATUS_FAILED, 0u);
        return;
    }
    rx_failSession(s_rx.target);     /* reports FAILED, marks slot EMPTY */
}

static void rx_onSwap(void)
{
    if (s_rx.state != OTARX_DONE)
    {
        return;                      /* nothing to swap into - ignore     */
    }
    if (s_rx.ops->systemReset != 0)
    {
        s_rx.ops->systemReset();     /* the SBL activates pending (§5.1)  */
    }
}

/* ---- public -------------------------------------------------------------------- */

void OTARX_init(const OtaRxOps *ops)
{
    memset(&s_rx, 0, sizeof(s_rx));
    s_rx.ops    = ops;
    s_rx.state  = OTARX_IDLE;
    s_rx.target = OTA_SLOT_NONE;
    s_rx.lastIdx = 0xFFFFu;
}

OtaRxState OTARX_state(void)
{
    return s_rx.state;
}

uint8_t OTARX_targetSlot(void)
{
    return s_rx.target;
}

uint8_t OTARX_progressPct(void)
{
    if (s_rx.state != OTARX_RECEIVING)
    {
        return (s_rx.state == OTARX_DONE) ? 100u : 0u;
    }
    return (uint8_t)((s_rx.rxTotal * 100u) / s_rx.totalLen);
}

uint32_t OTARX_framesSeen(void)
{
    return s_rx.frames;
}

void OTARX_frame(uint8_t sfType, uint8_t sfCid,
                 const uint8_t *data, uint16_t len)
{
    s_rx.frames++;

    if (sfType == SF_TYPE_OTA_DATA)
    {
        switch (sfCid)
        {
            case SF_CID_OTA_BEGIN: rx_onBegin(data, len); break;
            case SF_CID_OTA_CHUNK: rx_onChunk(data, len); break;
            case SF_CID_OTA_ABORT: rx_onAbort();          break;
            default: break;
        }
        return;
    }
    if (sfType == SF_TYPE_OTA_CTRL)
    {
        if (sfCid == SF_CID_OTA_SWAP)
        {
            rx_onSwap();
        }
        return;
    }
}
