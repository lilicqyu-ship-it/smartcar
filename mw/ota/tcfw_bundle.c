/*
 * tcfw_bundle.c - TCFW streaming verifier (doc 24 §6)
 *
 * Structure follows c6_car components/c6_ota/bundle.c closely (same feed
 * loop shape, signature-at-header, digest-at-finish); differences are the
 * TCFW magic, the single-payload layout, an explicit I/O error state for
 * flash write failures, and no heap - the caller owns the context.
 */
#include "tcfw_bundle.h"

#include <string.h>

#include "../crypto/ed25519v.h"

static uint32_t tcfw_getU16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static uint32_t tcfw_getU32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

const char *TCFW_stateStr(TcfwState st)
{
    switch (st)
    {
        case TCFW_NEED_HDR:     return "need-hdr";
        case TCFW_APP_DATA:     return "app-data";
        case TCFW_DONE:         return "done";
        case TCFW_ERR_MAGIC:    return "magic";
        case TCFW_ERR_VERSION:  return "version";
        case TCFW_ERR_SIG:      return "signature";
        case TCFW_ERR_SIZE:     return "size";
        case TCFW_ERR_HASH:     return "hash";
        case TCFW_ERR_TRUNCATED:return "truncated";
        case TCFW_ERR_IO:       return "io";
        default:                return "unknown";
    }
}

void TCFW_init(TcfwCtx *ctx, const uint8_t pubkey[32])
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->state = TCFW_NEED_HDR;
    ctx->pub   = pubkey;
}

/* Header complete: parse, size-check, verify the ed25519 signature over the
 * first 84 bytes. Only a header that passes everything switches the context
 * into payload mode. */
static TcfwState tcfw_parseHeader(TcfwCtx *ctx)
{
    const uint8_t *h = ctx->hdr;
    uint32_t total;
    uint32_t appLen;
    uint32_t rsvLen;
    uint32_t hdrLen;

    if (memcmp(h, "TCFW", 4u) != 0)
    {
        return TCFW_ERR_MAGIC;
    }
    if (h[4] != 1u)
    {
        return TCFW_ERR_VERSION;
    }
    hdrLen = tcfw_getU16(&h[6]);
    if (hdrLen != TCFW_HDR_LEN)
    {
        return TCFW_ERR_VERSION;
    }

    total   = tcfw_getU32(&h[8]);
    appLen  = tcfw_getU32(&h[12]);
    rsvLen  = tcfw_getU32(&h[48]);

    if ((appLen == 0u) || (appLen > TCFW_MAX_APP) || (rsvLen != 0u) ||
        (total != (TCFW_HDR_LEN + appLen)))
    {
        return TCFW_ERR_SIZE;
    }

    /* no key provisioned -> treat as signature failure, same as the C6 side */
    {
        uint8_t zeros[32];

        memset(zeros, 0, sizeof(zeros));
        if ((ctx->pub == 0) || (memcmp(ctx->pub, zeros, 32u) == 0))
        {
            return TCFW_ERR_SIG;
        }
    }
    if (c6_ed25519_verify(ctx->pub, &h[84], h, TCFW_SIGNED_LEN) != 0)
    {
        return TCFW_ERR_SIG;
    }

    ctx->info.app_len = appLen;
    ctx->info.flags   = h[5];
    memcpy(ctx->info.app_sha, &h[16], 32u);
    c6_sha512_init(&ctx->appHash);
    return TCFW_APP_DATA;
}

TcfwState TCFW_feed(TcfwCtx *ctx, const uint8_t *data, uint32_t len, void *arg,
                    int (*sink)(void *arg, uint32_t off,
                                const uint8_t *d, uint32_t n))
{
    if ((ctx == 0) || ((data == 0) && (len != 0u)))
    {
        return (ctx != 0) ? ctx->state : TCFW_ERR_SIZE;
    }
    if (ctx->state >= TCFW_DONE)
    {
        return ctx->state;               /* terminal, including all errors */
    }

    while (len > 0u)
    {
        if (ctx->state == TCFW_NEED_HDR)
        {
            uint32_t n = TCFW_HDR_LEN - ctx->hdrGot;

            if (n > len)
            {
                n = len;
            }
            memcpy(&ctx->hdr[ctx->hdrGot], data, n);
            ctx->hdrGot += n;
            data += n;
            len  -= n;
            if (ctx->hdrGot == TCFW_HDR_LEN)
            {
                ctx->state = tcfw_parseHeader(ctx);
                if (ctx->state >= TCFW_DONE)
                {
                    return ctx->state;
                }
            }
            continue;
        }

        /* TCFW_APP_DATA */
        {
            uint32_t remain = ctx->info.app_len - ctx->appGot;
            uint32_t n = (len < remain) ? len : remain;

            c6_sha512_update(&ctx->appHash, data, n);
            if ((sink != 0) && (sink(arg, ctx->appGot, data, n) != 0))
            {
                ctx->state = TCFW_ERR_IO;
                return ctx->state;
            }
            ctx->appGot += n;
            data += n;
            len  -= n;
            if (ctx->appGot == ctx->info.app_len)
            {
                ctx->state = TCFW_DONE;
                if (len > 0u)
                {
                    /* bytes past the declared image: leave them for the
                     * caller (ota_rx flags the length mismatch) */
                    break;
                }
            }
        }
    }
    return ctx->state;
}

TcfwState TCFW_finish(TcfwCtx *ctx)
{
    uint8_t digest[C6_SHA512_DIGEST_LEN];   /* payload hash = SHA-512[:32] */

    if (ctx == 0)
    {
        return TCFW_ERR_SIZE;
    }
    if (ctx->state != TCFW_DONE)
    {
        ctx->state = TCFW_ERR_TRUNCATED;
        return ctx->state;
    }
    c6_sha512_final(&ctx->appHash, digest);
    if (memcmp(digest, ctx->info.app_sha, 32u) != 0)
    {
        ctx->state = TCFW_ERR_HASH;
    }
    return ctx->state;
}
