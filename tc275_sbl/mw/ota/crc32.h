/*
 * crc32.h - CRC-32 (IEEE 802.3 / zlib), portable C99
 *
 * Used for two independent things in the OTA scheme:
 *   - the OtaMeta page integrity field (doc 24 §4)
 *   - the transport-level check value of an OTA transfer (OTA_BEGIN carries
 *     the CRC-32 of the whole TCFW bundle; doc 24 §5.3)
 * Same polynomial/init/final-xor as python's zlib.crc32, so the C6 side can
 * compute the expected value with the stock library.
 */
#ifndef OTA_CRC32_H
#define OTA_CRC32_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* First use builds the 256-entry table lazily; safe to call from any single
 * core context, and the SBL / App / host tests each have exactly one. */
void     crc32_init(void);

/* Streaming update. Feed crc32_init_value() on the first call, chain the
 * return value into the next. */
uint32_t crc32_init_value(void);
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);
uint32_t crc32_final(uint32_t crc);

/* One-shot. */
uint32_t crc32_buf(const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* OTA_CRC32_H */
