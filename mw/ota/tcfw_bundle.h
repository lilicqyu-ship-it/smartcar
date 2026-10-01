/*
 * tcfw_bundle.h - TC275 firmware bundle ("TCFW") streaming verifier (doc 24 §6)
 *
 * Layout mirrors the C6 "C6FW" bundle byte for byte so the existing signing
 * tooling and verification stack carry over; only the payload differs (one
 * TriCore App image instead of c6.bin+assets.bin):
 *
 *   0    magic "TCFW"
 *   4    u8  fmt_ver = 1
 *   5    u8  flags         bit0 = slot-agnostic image (informational; the
 *                          receiver always writes the non-active slot and the
 *                          boot-attempt rollback covers a wrong-slot push)
 *   6    u16 hdr_len = 148
 *   8    u32 total_len     whole bundle, == 148 + app_len
 *   12   u32 app_len       TriCore App image bytes
 *   16   32B app_sha       SHA-512 truncated to its first 32 bytes
 *   48   u32 rsv_len       reserved, must be 0
 *   52   32B rsv_sha       zeros
 *   84   64B ed25519 signature over [0, 84)
 *   148  app image ...
 *
 * Crypto facts pinned against the C6 side (doc 24 risk R2, resolved by
 * reading esp32c6_car code, not comments): the signature covers the FIRST 84
 * bytes (tools/sign_bundle.py SIGNED_LEN = 84; the "first 116 bytes" wording
 * in c6 bundle.h's header comment is stale), and payload digests are
 * SHA-512[:32] despite the field names saying sha256. This module matches
 * that exactly - byte-level compatible with sign_bundle.py-derived packers.
 *
 * The signature is checked as soon as the 148-byte header has streamed in
 * and BEFORE any payload byte is sunk, so flash writes only ever happen for
 * a correctly signed bundle.
 *
 * No dynamic memory: the caller owns the context (embedded in ota_rx on the
 * target). Pure C99.
 */
#ifndef TCFW_BUNDLE_H
#define TCFW_BUNDLE_H

#include <stdint.h>

#include "../crypto/sha512.h"
#include "ota_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TCFW_HDR_LEN        148u
#define TCFW_SIGNED_LEN     84u
#define TCFW_MAX_APP        OTA_SLOT_SIZE

typedef enum
{
    TCFW_NEED_HDR = 0,       /* accumulating the 148-byte header            */
    TCFW_APP_DATA,           /* header verified; streaming the image        */
    TCFW_DONE,               /* all bytes consumed (digests not yet checked)*/
    TCFW_ERR_MAGIC,
    TCFW_ERR_VERSION,
    TCFW_ERR_SIG,            /* ed25519 signature invalid                   */
    TCFW_ERR_SIZE,           /* declared sizes inconsistent / out of range  */
    TCFW_ERR_HASH,           /* payload digest mismatch (TcfwFinish)       */
    TCFW_ERR_TRUNCATED,      /* stream ended before total_len              */
    TCFW_ERR_IO              /* payload sink refused a write (flash error) */
} TcfwState;

typedef struct
{
    uint32_t app_len;
    uint8_t  app_sha[32];
    uint8_t  flags;
} TcfwInfo;

typedef struct
{
    TcfwState      state;
    TcfwInfo       info;
    const uint8_t *pub;             /* 32-byte ed25519 public key           */
    uint8_t        hdr[TCFW_HDR_LEN];
    uint32_t       hdrGot;
    uint32_t       appGot;
    c6_sha512_ctx_t appHash;
} TcfwCtx;

const char *TCFW_stateStr(TcfwState st);

/* pubkey = 32 bytes, owned by the caller, must outlive the context. */
void TCFW_init(TcfwCtx *ctx, const uint8_t pubkey[32]);

/* Stream bundle bytes. Payload bytes (the App image only) are forwarded to
 * sink(arg, off, data, n) with off counted from the start of the image;
 * the sink is only called after the header verified. Returns the new state;
 * error states are terminal and further feed() calls are ignored. */
TcfwState TCFW_feed(TcfwCtx *ctx, const uint8_t *data, uint32_t len, void *arg,
                    int (*sink)(void *arg, uint32_t off,
                                const uint8_t *d, uint32_t n));

/* Check the streamed image digest against the (signed) header field.
 * Only meaningful once state == TCFW_DONE. */
TcfwState TCFW_finish(TcfwCtx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TCFW_BUNDLE_H */
