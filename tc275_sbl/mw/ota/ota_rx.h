/*
 * ota_rx.h - SF OTA receiver state machine, TC275 side (doc 24 §5.3)
 *
 * Consumes decoded SF frames (the link layer already parsed type/cid/payload)
 * and drives a complete transfer:
 *
 *   IDLE
 *    + OTA_BEGIN{u32 total, u32 crc32}  -> pick non-active slot, erase it,
 *    |                                    state RECEIVING, STATUS{RUNNING,0}
 *   RECEIVING
 *    + OTA_CHUNK{u16 idx, data<=240}    -> sequential idx check, stream into
 *    |                                    the TCFW verifier (sig checked at
 *    |                                    its 148-byte header, before any
 *    |                                    flash write), ACK{idx,result} per
 *    |                                    chunk, STATUS{RUNNING,pct} on
 *    |                                    percent change
 *    + OTA_ABORT                         -> drop slot, state EMPTY, FAILED
 *    + last chunk                        -> whole-bundle CRC32 + bundle SHA +
 *    |                                    read-back SHA, then meta marks the
 *    |                                    slot DOWNLOADED, STATUS{DONE,100}
 *   DONE
 *    + OTA_SWAP                          -> soft reset; the SBL activates the
 *                                         pending slot (doc 24 §5.1)
 *
 * Everything target-specific is injected through OtaRxOps: the flash backend,
 * the metadata store, the frame sender (LINK_send on target) and the reset.
 * Pure C99 - the full state machine runs in the host unit tests (G-OTA-2 and
 * the doc 24 §5.3 ladder).
 *
 * ACK result codes and STATUS state codes are fixed wire values shared with
 * the C6 pusher (proto v2 0x62/0x63 payloads):
 *   ACK   {u16 idx LE, u8 result}  result: 0 ok, 1 bad idx, 2 flash, 3 busy,
 *                                  4 verify, 5 bad length
 *   STATUS{u8 state, u8 pct}       state: 0 idle, 1 running, 2 done, 3 failed
 */
#ifndef OTA_RX_H
#define OTA_RX_H

#include <stdint.h>

#include "ota_layout.h"
#include "tcfw_bundle.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OTARX_CHUNK_MAX         240u   /* doc 22 SS5.5 upper bound            */

/* STATUS state values */
#define OTARX_STATUS_IDLE       0u
#define OTARX_STATUS_RUNNING    1u
#define OTARX_STATUS_DONE       2u
#define OTARX_STATUS_FAILED     3u

/* ACK result values */
#define OTARX_ACK_OK            0u
#define OTARX_ACK_BAD_IDX       1u
#define OTARX_ACK_FLASH         2u
#define OTARX_ACK_BUSY          3u
#define OTARX_ACK_VERIFY        4u
#define OTARX_ACK_BAD_LEN       5u

typedef enum
{
    OTARX_IDLE = 0,
    OTARX_RECEIVING,
    OTARX_DONE
} OtaRxState;

typedef struct
{
    /* which slot the running App occupies (0/1); the transfer always
     * targets the other one */
    uint8_t activeSlot;

    /* 32-byte ed25519 public key for TCFW bundles */
    const uint8_t *pubkey;

    /* Erase the whole target slot. 1 = ok. */
    uint8_t (*eraseSlot)(uint8_t slot);
    /* Sequential payload write into the erased slot (backend stages
     * partial PFlash pages internally). 1 = ok. */
    uint8_t (*writeApp)(uint8_t slot, uint32_t off,
                        const uint8_t *data, uint32_t len);
    /* Program any staged tail (pad 0xFF). 1 = ok. */
    uint8_t (*flushApp)(uint8_t slot, uint32_t written);
    /* Read back written bytes for the post-flash digest check. 1 = ok. */
    uint8_t (*readApp)(uint8_t slot, uint32_t off, uint32_t len, uint8_t *out);

    /* Metadata store hooks (drive OTAMETA + OTABOOT on target). */
    void (*metaDownloaded)(uint8_t slot);
    void (*metaAborted)(uint8_t slot);

    /* End of a successful SWAP: reset the MCU. Never returns. */
    void (*systemReset)(void);

    /* Queue one SF frame for TX (type/cid/payload exactly as on the wire). */
    void (*send)(uint8_t sfType, uint8_t sfCid,
                 const uint8_t *payload, uint8_t len);
} OtaRxOps;

/* Wire the ops (must stay valid for the lifetime of the receiver). */
void OTARX_init(const OtaRxOps *ops);

OtaRxState OTARX_state(void);
uint8_t    OTARX_targetSlot(void);   /* OTA_SLOT_NONE while IDLE */
uint8_t    OTARX_progressPct(void);  /* 0..100, snapshot for STATUS          */

/* Feed one decoded SF frame. Accepts TYPE_OTA_DATA (BEGIN/CHUNK/ABORT) and
 * TYPE_OTA_CTRL (SWAP); anything else is ignored. Payload lifetime is the
 * caller's (copied or consumed synchronously). */
void OTARX_frame(uint8_t sfType, uint8_t sfCid,
                 const uint8_t *data, uint16_t len);

/* Number of OTA frames handled; diagnostics counter for bench health. */
uint32_t OTARX_framesSeen(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_RX_H */
