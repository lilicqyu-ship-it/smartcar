/*
 * mock_ota.h - host stand-in for bsp/flash_ota.c + the target glue
 *
 * Simulates the double DFlash meta page and the two PFlash slots as RAM
 * arrays with real erase semantics (0xFF fill), plus failure injection and
 * call recording so the tests can assert on the protocol, not on hardware.
 */
#ifndef MOCK_OTA_H
#define MOCK_OTA_H

#include <stdint.h>

#include "mw/ota/ota_meta.h"
#include "mw/ota/ota_rx.h"

#ifdef __cplusplus
extern "C" {
#endif

void MOCK_reset(void);                    /* blank flash, clear counters      */

/* ---- injected state ---------------------------------------------------------- */

extern uint8_t mock_metaEraseFail;        /* 1 -> meta erase page fails       */
extern uint8_t mock_metaWriteFail;        /* 1 -> meta write page fails       */
extern uint8_t mock_slotEraseFail;        /* 1 -> eraseSlot fails             */
extern uint8_t mock_slotWriteFail;        /* 1 -> writeApp fails              */

/* ---- observation -------------------------------------------------------------- */

extern uint32_t mock_metaErases;          /* page erases issued               */
extern uint32_t mock_slotErases;          /* slot erase calls                 */
extern uint32_t mock_metaDownloadedCalls;
extern uint32_t mock_metaAbortedCalls;
extern uint32_t mock_resetCalls;
extern uint8_t  mock_metaDownloadedSlot;
extern uint8_t  mock_metaAbortedSlot;

typedef struct
{
    uint8_t  type;
    uint8_t  cid;
    uint8_t  len;
    uint8_t  payload[16];
} MockTxFrame;

#define MOCK_TXQ_MAX 1024
extern MockTxFrame mock_txq[MOCK_TXQ_MAX];
extern uint32_t    mock_txCount;

void mock_txClear(void);

/* ---- flash images (test inspection) -------------------------------------------- */

extern uint8_t mock_metaPages[OTA_META_PAGES][OTA_META_PAGE_SIZE];
extern uint8_t mock_slots[2][4096 * 260];   /* 1040 KB each: enough for the
                                             * 65 KB test image; slot ops
                                             * bounds-check against this     */
extern uint32_t mock_slotSize;

/* Which physical meta page holds the currently valid data (post-corruption
 * inspection). Returns 0/1, or 2 when neither page decodes. */
uint8_t mock_metaValidPages(uint8_t ok[OTA_META_PAGES]);

/* ---- backends exposed to the tests ---------------------------------------------- */

const OtaMetaBackend *mock_metaBackend(void);

/* OTA receiver ops with activeSlot fixed by the test before OTARX_init. */
extern uint8_t mock_activeSlot;
const OtaRxOps *mock_rxOps(void);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_OTA_H */
