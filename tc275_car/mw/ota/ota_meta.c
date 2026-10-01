/*
 * ota_meta.c - OtaMeta wire codec + double-page store (doc 24 §4)
 */
#include "ota_meta.h"

#include <string.h>

#include "crc32.h"

/* ---- LE field helpers (TriCore is big-endian; never cast the struct) ------- */

static void meta_putU32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t meta_getU32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ---- codec ------------------------------------------------------------------ */

void OTAMETA_encode(const OtaMeta *m, uint8_t wire[OTA_META_WIRE_LEN])
{
    wire[0]  = 'T';
    wire[1]  = 'C';
    wire[2]  = 'O';
    wire[3]  = 'M';
    meta_putU32(&wire[4], m->seq);
    wire[8]  = m->active_slot;
    wire[9]  = m->pending_slot;
    wire[10] = m->slot_a_state;
    wire[11] = m->slot_b_state;
    wire[12] = m->boot_attempts;
    wire[13] = m->rsv[0];
    wire[14] = m->rsv[1];
    wire[15] = m->rsv[2];
    /* bytes 16..19 stay reserved-zero on the wire */
    wire[16] = 0u;
    wire[17] = 0u;
    wire[18] = 0u;
    wire[19] = 0u;
    meta_putU32(&wire[20], crc32_buf(wire, 20u));
}

uint8_t OTAMETA_decode(const uint8_t wire[OTA_META_WIRE_LEN], OtaMeta *m)
{
    if ((wire[0] != (uint8_t)'T') || (wire[1] != (uint8_t)'C') ||
        (wire[2] != (uint8_t)'O') || (wire[3] != (uint8_t)'M'))
    {
        return 0u;
    }
    if (crc32_buf(wire, 20u) != meta_getU32(&wire[20]))
    {
        return 0u;
    }

    memset(m, 0, sizeof(*m));
    m->magic         = OTA_META_MAGIC;
    m->seq           = meta_getU32(&wire[4]);
    m->active_slot   = wire[8];
    m->pending_slot  = wire[9];
    m->slot_a_state  = wire[10];
    m->slot_b_state  = wire[11];
    m->boot_attempts = wire[12];
    return 1u;
}

void OTAMETA_setDefault(OtaMeta *m, uint8_t activeSlot)
{
    memset(m, 0, sizeof(*m));
    m->magic        = OTA_META_MAGIC;
    m->seq          = 0u;
    m->active_slot  = (activeSlot == OTA_SLOT_B) ? OTA_SLOT_B : OTA_SLOT_A;
    m->pending_slot = OTA_SLOT_NONE;
    m->slot_a_state = (m->active_slot == OTA_SLOT_A) ? OTA_SSTATE_VALID
                                                     : OTA_SSTATE_EMPTY;
    m->slot_b_state = (m->active_slot == OTA_SLOT_B) ? OTA_SSTATE_VALID
                                                     : OTA_SSTATE_EMPTY;
    m->boot_attempts = 0u;
}

/* ---- double-page store -------------------------------------------------------- */

static const OtaMetaBackend *s_bk;
static OtaMeta    s_meta;
static uint8_t    s_page;      /* page the loaded revision came from          */
static uint8_t    s_valid;     /* a CRC-good revision is loaded               */

void OTAMETA_attach(const OtaMetaBackend *backend)
{
    s_bk = backend;
    OTAMETA_reset();
}

void OTAMETA_reset(void)
{
    s_valid = 0u;
    s_page  = 0u;
    memset(&s_meta, 0, sizeof(s_meta));
}

uint8_t OTAMETA_load(void)
{
    uint8_t wire[OTA_META_WIRE_LEN];
    OtaMeta cand;
    uint32_t bestSeq = 0u;
    uint8_t  haveBest = 0u;
    uint8_t  p;

    s_valid = 0u;
    if (s_bk == 0)
    {
        return 0u;
    }

    for (p = 0u; p < OTA_META_PAGES; p++)
    {
        s_bk->read(p, wire, OTA_META_WIRE_LEN);
        if (OTAMETA_decode(wire, &cand) == 0u)
        {
            continue;
        }
        /* strictly newer wins; on a seq tie the first page scanned (0) is
         * kept, which makes the choice deterministic everywhere */
        if ((haveBest == 0u) || (cand.seq > bestSeq))
        {
            s_meta   = cand;
            s_page   = p;
            s_valid  = 1u;
            bestSeq  = cand.seq;
            haveBest = 1u;
        }
    }
    return s_valid;
}

const OtaMeta *OTAMETA_get(void)
{
    return &s_meta;
}

uint8_t OTAMETA_currentPage(void)
{
    return s_page;
}

uint8_t OTAMETA_commit(const OtaMeta *next)
{
    uint8_t wire[OTA_META_WIRE_LEN];
    uint8_t back[OTA_META_WIRE_LEN];
    OtaMeta staged;
    uint8_t target;

    if ((s_bk == 0) || (next == 0))
    {
        return 0u;
    }

    staged = *next;
    staged.magic = OTA_META_MAGIC;
    staged.seq   = (s_valid != 0u) ? (s_meta.seq + 1u) : 1u;
    if (staged.seq == 0u)
    {
        /* u32 wrap after ~4e9 commits: refuse rather than reuse a seq */
        return 0u;
    }

    /* Target the inactive page; with no valid revision either page works. */
    target = (s_valid != 0u) ? (uint8_t)(s_page ^ 1u) : 0u;

    if (s_bk->erase(target) == 0u)
    {
        return 0u;
    }
    OTAMETA_encode(&staged, wire);
    if (s_bk->write(target, wire, OTA_META_WIRE_LEN) == 0u)
    {
        return 0u;
    }
    s_bk->read(target, back, OTA_META_WIRE_LEN);
    if (memcmp(wire, back, OTA_META_WIRE_LEN) != 0)
    {
        return 0u;
    }

    /* Committed: the new page is authoritative, the old one becomes the
     * shadow (older seq, still CRC-valid - free extra redundancy). */
    s_meta  = staged;
    s_page  = target;
    s_valid = 1u;
    return 1u;
}
