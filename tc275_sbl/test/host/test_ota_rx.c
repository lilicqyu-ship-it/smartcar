/*
 * test_ota_rx.c - doc 24 §5.3 receiver state machine over the mock backend,
 * driving it with the generated signed bundle
 */
#include "test.h"

#include <string.h>

#include "mock_ota.h"
#include "mw/ota/ota_rx.h"
#include "mw/sf/sf_frame.h"
#include "test_vectors.h"

/* frame helpers ----------------------------------------------------------- */

static void sendBegin(uint32_t total, uint32_t crc)
{
    uint8_t p[8];

    SF_putU32(&p[0], total);
    SF_putU32(&p[4], crc);
    OTARX_frame(0x06u, 0x30u, p, 8u);
}

static void sendChunk(uint16_t idx, const uint8_t *d, uint16_t n)
{
    uint8_t p[2 + 248];

    p[0] = (uint8_t)(idx & 0xFFu);
    p[1] = (uint8_t)(idx >> 8);
    memcpy(&p[2], d, n);
    OTARX_frame(0x06u, 0x31u, p, (uint16_t)(2u + n));
}

static void sendAbort(void)
{
    OTARX_frame(0x06u, 0x35u, 0, 0u);
}

static void sendSwap(void)
{
    OTARX_frame(0x07u, 0x34u, 0, 0u);
}

/* stream the whole bundle in 240-byte chunks, like the C6 pusher would */
static void streamBundle(const uint8_t *bundle, uint32_t len)
{
    uint32_t off = 0u;
    uint16_t idx = 0u;

    while (off < len)
    {
        uint16_t n = (uint16_t)((len - off) > 240u ? 240u : (len - off));

        sendChunk(idx, &bundle[off], n);
        off += n;
        idx++;
    }
}

/* the i-th ACK in the TX queue, or 0 */
static const MockTxFrame *findTxCid(uint8_t cid, uint32_t i)
{
    uint32_t seen = 0u;
    uint32_t k;

    for (k = 0u; k < mock_txCount; k++)
    {
        if (mock_txq[k].cid == cid)
        {
            if (seen == i)
            {
                return &mock_txq[k];
            }
            seen++;
        }
    }
    return 0;
}

static uint32_t countTxCid(uint8_t cid)
{
    uint32_t k;
    uint32_t n = 0u;

    for (k = 0u; k < mock_txCount; k++)
    {
        if (mock_txq[k].cid == cid)
        {
            n++;
        }
    }
    return n;
}

static void startSession(void)
{
    sendBegin(TV_BUNDLE_LEN, TV_BUNDLE_CRC32);
}

/* --------------------------------------------------------------------------- */

int main(void)
{
    /* 1. happy path: BEGIN -> chunks -> verify -> DONE, then SWAP resets */
    T_CASE("happy path");
    MOCK_reset();
    {
        OtaRxOps ops;

        ops = *mock_rxOps();
        ops.pubkey = tv_pub;
        OTARX_init(&ops);
        T_CHECK_EQ_I(OTARX_state(), OTARX_IDLE);

        startSession();
        T_CHECK_EQ_I(OTARX_state(), OTARX_RECEIVING);
        T_CHECK_EQ_I(OTARX_targetSlot(), OTA_SLOT_B);   /* active A -> B   */
        T_CHECK_EQ_I(mock_slotErases, 1u);
        T_CHECK_EQ_I(mock_txq[0].cid, 0x33u);           /* STATUS RUNNING  */
        T_CHECK_EQ_I(mock_txq[0].payload[0], OTARX_STATUS_RUNNING);
        T_CHECK_EQ_I(mock_txq[0].payload[1], 0u);

        streamBundle(tv_bundle, TV_BUNDLE_LEN);

        T_CHECK_EQ_I(OTARX_state(), OTARX_DONE);
        T_CHECK_EQ_I(mock_metaDownloadedCalls, 1u);
        T_CHECK_EQ_I(mock_metaDownloadedSlot, OTA_SLOT_B);
        T_CHECK_EQ_I(mock_metaAbortedCalls, 0u);
        /* the image landed in the slot verbatim */
        T_CHECK(memcmp(mock_slots[OTA_SLOT_B], &tv_bundle[148u], TV_APP_LEN) == 0);
        /* last ACK ok, final STATUS DONE 100 */
        {
            uint32_t acks = countTxCid(0x32u);

            T_CHECK(acks > 200u);                       /* one per chunk    */
            T_CHECK_EQ_I(SF_getU16(findTxCid(0x32u, acks - 1u)->payload), acks - 1u);
            T_CHECK_EQ_I(findTxCid(0x32u, acks - 1u)->payload[2], OTARX_ACK_OK);
            T_CHECK_EQ_I(findTxCid(0x33u, countTxCid(0x33u) - 1u)->payload[0],
                         OTARX_STATUS_DONE);
            T_CHECK_EQ_I(findTxCid(0x33u, countTxCid(0x33u) - 1u)->payload[1], 100u);
        }

        sendSwap();
        T_CHECK_EQ_I(mock_resetCalls, 1u);
    }

    /* 2. duplicate chunk is re-ACKed, not re-written */
    T_CASE("duplicate chunk");
    MOCK_reset();
    {
        OtaRxOps ops;

        ops = *mock_rxOps();
        ops.pubkey = tv_pub;
        OTARX_init(&ops);
        startSession();
        sendChunk(0u, tv_bundle, 240u);
        sendChunk(0u, tv_bundle, 240u);                 /* exact retransmit */
        T_CHECK_EQ_I(OTARX_state(), OTARX_RECEIVING);
        T_CHECK_EQ_I(findTxCid(0x32u, 1u)->payload[2], OTARX_ACK_OK);
        sendChunk(1u, &tv_bundle[240u], 240u);          /* stream continues */
        T_CHECK_EQ_I(findTxCid(0x32u, 2u)->payload[2], OTARX_ACK_OK);
    }

    /* 3. out-of-order idx: BAD_IDX ack + session fails */
    T_CASE("bad idx");
    MOCK_reset();
    {
        OtaRxOps ops;

        ops = *mock_rxOps();
        ops.pubkey = tv_pub;
        OTARX_init(&ops);
        startSession();
        sendChunk(0u, tv_bundle, 240u);
        sendChunk(5u, &tv_bundle[240u], 240u);
        T_CHECK_EQ_I(OTARX_state(), OTARX_IDLE);
        {
            const MockTxFrame *ack = findTxCid(0x32u, 1u);

            T_CHECK(ack != 0);
            T_CHECK_EQ_I(SF_getU16(ack->payload), 5u);
            T_CHECK_EQ_I(ack->payload[2], OTARX_ACK_BAD_IDX);
        }
        T_CHECK_EQ_I(mock_metaAbortedCalls, 1u);
        T_CHECK_EQ_I(mock_metaAbortedSlot, OTA_SLOT_B);
        T_CHECK_EQ_I(findTxCid(0x33u, countTxCid(0x33u) - 1u)->payload[0],
                     OTARX_STATUS_FAILED);
    }

    /* 4. wrong transport CRC: everything streams, final check fails */
    T_CASE("bad total crc");
    MOCK_reset();
    {
        OtaRxOps ops;

        ops = *mock_rxOps();
        ops.pubkey = tv_pub;
        OTARX_init(&ops);
        sendBegin(TV_BUNDLE_LEN, TV_BUNDLE_CRC32 ^ 1u);
        streamBundle(tv_bundle, TV_BUNDLE_LEN);
        T_CHECK_EQ_I(OTARX_state(), OTARX_IDLE);
        T_CHECK_EQ_I(mock_metaDownloadedCalls, 0u);
        T_CHECK_EQ_I(mock_metaAbortedCalls, 1u);
        T_CHECK_EQ_I(findTxCid(0x33u, countTxCid(0x33u) - 1u)->payload[0],
                     OTARX_STATUS_FAILED);
    }

    /* 5. signature failure at the header chunk: no payload write happens */
    T_CASE("bad signature");
    MOCK_reset();
    {
        OtaRxOps ops;
        uint32_t i;

        ops = *mock_rxOps();
        ops.pubkey = tv_pub;
        OTARX_init(&ops);
        sendBegin(TV_BUNDLE_LEN, 0x12345678u);         /* crc checked later */
        /* first chunk carries the header; tamper a signed byte */
        {
            uint8_t first[240];

            memcpy(first, tv_wrongkey, 240u);
            sendChunk(0u, first, 240u);
        }
        T_CHECK_EQ_I(OTARX_state(), OTARX_IDLE);
        /* no image byte may have been sunk: slot still erased past page 0?
         * the mock writes only after the header verifies, so nothing at all */
        for (i = 0u; i < 64u; i++)
        {
            T_CHECK_EQ_I(mock_slots[OTA_SLOT_B][i], 0xFFu);
        }
        T_CHECK_EQ_I(mock_metaAbortedCalls, 1u);
    }

    /* 6. flash write failure mid-stream */
    T_CASE("flash write failure");
    MOCK_reset();
    {
        OtaRxOps ops;

        ops = *mock_rxOps();
        ops.pubkey = tv_pub;
        OTARX_init(&ops);
        startSession();
        sendChunk(0u, tv_bundle, 240u);
        mock_slotWriteFail = 1u;
        sendChunk(1u, &tv_bundle[240u], 240u);
        T_CHECK_EQ_I(OTARX_state(), OTARX_IDLE);
        {
            const MockTxFrame *ack = findTxCid(0x32u, 1u);

            T_CHECK(ack != 0);
            T_CHECK_EQ_I(ack->payload[2], OTARX_ACK_FLASH);
        }
    }

    /* 7. erase failure at BEGIN */
    T_CASE("erase failure");
    MOCK_reset();
    {
        OtaRxOps ops;

        ops = *mock_rxOps();
        ops.pubkey = tv_pub;
        OTARX_init(&ops);
        mock_slotEraseFail = 1u;
        startSession();
        T_CHECK_EQ_I(OTARX_state(), OTARX_IDLE);
        T_CHECK_EQ_I(findTxCid(0x33u, 0u)->payload[0], OTARX_STATUS_FAILED);
    }

    /* 8. ABORT mid-transfer drops the slot */
    T_CASE("abort");
    MOCK_reset();
    {
        OtaRxOps ops;

        ops = *mock_rxOps();
        ops.pubkey = tv_pub;
        OTARX_init(&ops);
        startSession();
        sendChunk(0u, tv_bundle, 240u);
        sendAbort();
        T_CHECK_EQ_I(OTARX_state(), OTARX_IDLE);
        T_CHECK_EQ_I(OTARX_targetSlot(), OTA_SLOT_NONE);
        T_CHECK_EQ_I(mock_metaAbortedCalls, 1u);
        T_CHECK_EQ_I(mock_metaAbortedSlot, OTA_SLOT_B);
        /* chunk after abort is answered BUSY */
        sendChunk(1u, &tv_bundle[240u], 240u);
        T_CHECK_EQ_I(findTxCid(0x32u, 1u)->payload[2], OTARX_ACK_BUSY);
    }

    /* 9. SWAP before DONE is ignored (no reset) */
    T_CASE("early swap ignored");
    MOCK_reset();
    {
        OtaRxOps ops;

        ops = *mock_rxOps();
        ops.pubkey = tv_pub;
        OTARX_init(&ops);
        startSession();
        sendSwap();
        T_CHECK_EQ_I(mock_resetCalls, 0u);
        T_CHECK_EQ_I(OTARX_state(), OTARX_RECEIVING);
    }

    /* 10. BEGIN while receiving answers RUNNING with current pct */
    T_CASE("begin while running");
    MOCK_reset();
    {
        OtaRxOps ops;

        ops = *mock_rxOps();
        ops.pubkey = tv_pub;
        OTARX_init(&ops);
        startSession();
        sendChunk(0u, tv_bundle, 240u);
        startSession();
        T_CHECK_EQ_I(OTARX_state(), OTARX_RECEIVING);
        T_CHECK_EQ_I(mock_slotErases, 1u);              /* no re-erase      */
    }

    /* 11. nonsense total length is refused */
    T_CASE("oversize begin");
    MOCK_reset();
    {
        OtaRxOps ops;

        ops = *mock_rxOps();
        ops.pubkey = tv_pub;
        OTARX_init(&ops);
        sendBegin(0xFFFFFFFFu, 0u);
        T_CHECK_EQ_I(OTARX_state(), OTARX_IDLE);
        T_CHECK_EQ_I(mock_slotErases, 0u);
        T_CHECK_EQ_I(findTxCid(0x33u, 0u)->payload[0], OTARX_STATUS_FAILED);
    }

    /* 12. active B -> target A (slot choice mirrors the active one) */
    T_CASE("active B targets A");
    MOCK_reset();
    {
        OtaRxOps ops;

        mock_activeSlot = OTA_SLOT_B;
        ops = *mock_rxOps();
        ops.pubkey = tv_pub;
        OTARX_init(&ops);
        startSession();
        T_CHECK_EQ_I(OTARX_targetSlot(), OTA_SLOT_A);
    }

    T_RESULT("test_ota_rx");
}
