/*
 * ota_meta.h - OTA metadata, DFlash double-page committed (doc 24 §4)
 *
 * One OtaMeta record describes the whole boot state of the two App slots.
 * It is stored twice in DFlash0 (sectors 13/14, see ota_layout.h) and
 * alternates between the two pages on every commit: the new revision is
 * written to the currently-inactive page with seq+1, verified by read-back,
 * and only then becomes authoritative. A power cut mid-commit can damage the
 * new page only; the old page is never touched and wins by CRC + seq
 * ordering on the next load (doc 24 gate G-OTA-6).
 *
 * Wire format: 24 bytes, all multi-byte fields LITTLE-ENDIAN on the wire
 * (TriCore is big-endian; never memcpy the struct over flash bytes). The
 * magic bytes read 'T','C','O','M' at increasing addresses.
 *
 * Pure C99 + injected flash backend, so the same logic runs in the SBL
 * (TASKING), the App, and the host unit tests.
 */
#ifndef OTA_META_H
#define OTA_META_H

#include <stdint.h>
#include "ota_layout.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- slot states (doc 24 §4, esp_ota semantics) --------------------------- */

#define OTA_SSTATE_EMPTY          0u
#define OTA_SSTATE_WRITING        1u
#define OTA_SSTATE_DOWNLOADED     2u   /* image fully received + verified      */
#define OTA_SSTATE_PENDING_VERIFY 3u   /* SBL will boot it once (or N times)   */
#define OTA_SSTATE_VALID          4u   /* App confirmed itself                 */
#define OTA_SSTATE_INVALID        5u   /* failed self-verify / rolled back over */

/* ---- logical record -------------------------------------------------------- */

typedef struct
{
    uint32_t magic;          /* OTA_META_MAGIC                            */
    uint32_t seq;            /* monotonic; larger valid page wins          */
    uint8_t  active_slot;    /* 0=A 1=B - what the SBL boots               */
    uint8_t  pending_slot;   /* OTA target while/after download, OTA_SLOT_NONE */
    uint8_t  slot_a_state;   /* OTA_SSTATE_*                               */
    uint8_t  slot_b_state;
    uint8_t  boot_attempts;  /* SBL increments per PENDING_VERIFY boot     */
    uint8_t  rsv[3];
    uint32_t crc32;          /* over the first 20 wire bytes               */
} OtaMeta;

/* ---- wire codec ------------------------------------------------------------ */

/* Serialize to exactly OTA_META_WIRE_LEN little-endian bytes. */
void    OTAMETA_encode(const OtaMeta *m, uint8_t wire[OTA_META_WIRE_LEN]);

/* Parse + integrity check. Returns 1 when magic and CRC32 both pass. */
uint8_t OTAMETA_decode(const uint8_t wire[OTA_META_WIRE_LEN], OtaMeta *m);

/* A bootable default for a board whose DFlash has never been written:
 * given active slot marked VALID. Callers normally do NOT commit this -
 * it exists so first-boot / factory flows have a coherent starting point. */
void    OTAMETA_setDefault(OtaMeta *m, uint8_t activeSlot);

/* ---- flash backend (implemented by bsp/flash_ota.c on target, mock on host) */

typedef struct
{
    /* Erase the 8 KB DFlash sector backing meta page pageIdx. 1 = ok. */
    uint8_t (*erase)(uint8_t pageIdx);
    /* Program len (<= OTA_META_WIRE_LEN) bytes at the start of the page.
     * The page is freshly erased when this is called. 1 = ok. */
    uint8_t (*write)(uint8_t pageIdx, const uint8_t *data, uint32_t len);
    /* Read len bytes from the start of the page. */
    void    (*read)(uint8_t pageIdx, uint8_t *data, uint32_t len);
} OtaMetaBackend;

/* ---- double-page store ------------------------------------------------------ */

/* Attach the backend and (re)load the store. One store per process is enough
 * for SBL, App and tests alike; tests reset it between cases. */
void    OTAMETA_attach(const OtaMetaBackend *backend);
void    OTAMETA_reset(void);            /* drop state, keep backend           */

/* Read both pages, pick the CRC-valid one with the higher seq.
 * Returns 1 when any valid revision exists. */
uint8_t OTAMETA_load(void);

/* Copy of the currently loaded revision (undefined content when load failed). */
const OtaMeta *OTAMETA_get(void);

/* Which physical page the loaded revision came from (0/1, 0 when invalid). */
uint8_t OTAMETA_currentPage(void);

/* Commit a new revision: bumps seq relative to the loaded one, writes the
 * inactive page, verifies by read-back. The previously active page is left
 * as-is (older seq = shadow copy). Returns 1 on success, after which
 * OTAMETA_get() returns the new revision. */
uint8_t OTAMETA_commit(const OtaMeta *next);

#ifdef __cplusplus
}
#endif

#endif /* OTA_META_H */
