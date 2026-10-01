/*
 * mock_ota.c - host flash/backend simulation for the OTA unit tests
 */
#include "mock_ota.h"

#include <string.h>

#include "mw/ota/ota_layout.h"

uint8_t  mock_metaEraseFail;
uint8_t  mock_metaWriteFail;
uint8_t  mock_slotEraseFail;
uint8_t  mock_slotWriteFail;

uint32_t mock_metaErases;
uint32_t mock_slotErases;
uint32_t mock_metaDownloadedCalls;
uint32_t mock_metaAbortedCalls;
uint32_t mock_resetCalls;
uint8_t  mock_metaDownloadedSlot = 0xFFu;
uint8_t  mock_metaAbortedSlot    = 0xFFu;

MockTxFrame mock_txq[MOCK_TXQ_MAX];
uint32_t    mock_txCount;

uint8_t  mock_metaPages[OTA_META_PAGES][OTA_META_PAGE_SIZE];
uint8_t  mock_slots[2][4096 * 260];
uint32_t mock_slotSize = 4096u * 260u;
uint8_t  mock_activeSlot = OTA_SLOT_A;

void MOCK_reset(void)
{
    memset(mock_metaPages, 0xFF, sizeof(mock_metaPages));
    memset(mock_slots, 0xFF, sizeof(mock_slots));
    mock_metaEraseFail = mock_metaWriteFail = 0u;
    mock_slotEraseFail = mock_slotWriteFail = 0u;
    mock_metaErases = 0u;
    mock_slotErases = 0u;
    mock_metaDownloadedCalls = 0u;
    mock_metaAbortedCalls = 0u;
    mock_resetCalls = 0u;
    mock_metaDownloadedSlot = 0xFFu;
    mock_metaAbortedSlot = 0xFFu;
    mock_activeSlot = OTA_SLOT_A;
    mock_txClear();
}

void mock_txClear(void)
{
    mock_txCount = 0u;
}

uint8_t mock_metaValidPages(uint8_t ok[OTA_META_PAGES])
{
    OtaMeta m;
    uint8_t any = 0u;
    uint8_t p;

    for (p = 0u; p < OTA_META_PAGES; p++)
    {
        ok[p] = OTAMETA_decode(mock_metaPages[p], &m);
        if (ok[p] != 0u)
        {
            any = 1u;
        }
    }
    return any;
}

/* ---- OtaMeta backend ----------------------------------------------------------- */

static uint8_t mock_metaErase(uint8_t pageIdx)
{
    mock_metaErases++;
    if (mock_metaEraseFail != 0u)
    {
        return 0u;
    }
    memset(mock_metaPages[pageIdx], 0xFF, OTA_META_PAGE_SIZE);
    return 1u;
}

static uint8_t mock_metaWrite(uint8_t pageIdx, const uint8_t *data, uint32_t len)
{
    if ((mock_metaWriteFail != 0u) || (len > OTA_META_PAGE_SIZE))
    {
        return 0u;
    }
    /* real flash only clears bits; emulate that a second write onto the same
     * page without erase would corrupt - tests always erase first */
    memcpy(mock_metaPages[pageIdx], data, len);
    return 1u;
}

static void mock_metaRead(uint8_t pageIdx, uint8_t *data, uint32_t len)
{
    memcpy(data, mock_metaPages[pageIdx], len);
}

static const OtaMetaBackend s_metaBackend =
{
    mock_metaErase,
    mock_metaWrite,
    mock_metaRead
};

const OtaMetaBackend *mock_metaBackend(void)
{
    return &s_metaBackend;
}

/* ---- OtaRx ops ------------------------------------------------------------------- */

static uint8_t mock_eraseSlot(uint8_t slot)
{
    mock_slotErases++;
    if (mock_slotEraseFail != 0u)
    {
        return 0u;
    }
    memset(mock_slots[slot], 0xFF, mock_slotSize);
    return 1u;
}

/* simple byte-granular "flash": the mock skips PFlash page staging (that is
 * target backend behaviour); the receiver contract only needs a sequential
 * byte store */
static uint8_t mock_writeApp(uint8_t slot, uint32_t off,
                             const uint8_t *data, uint32_t len)
{
    if (mock_slotWriteFail != 0u)
    {
        return 0u;
    }
    if ((slot > 1u) || (off + len > mock_slotSize))
    {
        return 0u;
    }
    memcpy(&mock_slots[slot][off], data, len);
    return 1u;
}

static uint8_t mock_flushApp(uint8_t slot, uint32_t written)
{
    (void)slot;
    (void)written;
    return 1u;
}

static uint8_t mock_readApp(uint8_t slot, uint32_t off, uint32_t len, uint8_t *out)
{
    if ((slot > 1u) || (off + len > mock_slotSize))
    {
        return 0u;
    }
    memcpy(out, &mock_slots[slot][off], len);
    return 1u;
}

static void mock_metaDownloaded(uint8_t slot)
{
    mock_metaDownloadedCalls++;
    mock_metaDownloadedSlot = slot;
}

static void mock_metaAborted(uint8_t slot)
{
    mock_metaAbortedCalls++;
    mock_metaAbortedSlot = slot;
}

static void mock_systemReset(void)
{
    mock_resetCalls++;
}

static void mock_send(uint8_t type, uint8_t cid,
                      const uint8_t *payload, uint8_t len)
{
    if (mock_txCount < MOCK_TXQ_MAX)
    {
        MockTxFrame *f = &mock_txq[mock_txCount++];

        f->type = type;
        f->cid  = cid;
        f->len  = len;
        if (len > (uint8_t)sizeof(f->payload))
        {
            len = (uint8_t)sizeof(f->payload);
        }
        memcpy(f->payload, payload, len);
    }
}

static OtaRxOps s_rxOps;

const OtaRxOps *mock_rxOps(void)
{
    s_rxOps.activeSlot     = mock_activeSlot;
    s_rxOps.pubkey         = 0;      /* filled in by the test */
    s_rxOps.eraseSlot      = mock_eraseSlot;
    s_rxOps.writeApp       = mock_writeApp;
    s_rxOps.flushApp       = mock_flushApp;
    s_rxOps.readApp        = mock_readApp;
    s_rxOps.metaDownloaded = mock_metaDownloaded;
    s_rxOps.metaAborted    = mock_metaAborted;
    s_rxOps.systemReset    = mock_systemReset;
    s_rxOps.send           = mock_send;
    return &s_rxOps;
}
