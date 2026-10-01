/*
 * crc32.c - CRC-32 (IEEE 802.3 / zlib): poly 0xEDB88320 (reflected),
 * init 0xFFFFFFFF, final xor 0xFFFFFFFF. check("123456789") == 0xCBF43926.
 */
#include "crc32.h"

static uint32_t s_table[256];
static uint8_t  s_tableReady;

void crc32_init(void)
{
    uint32_t i;

    if (s_tableReady != 0u)
    {
        return;
    }
    for (i = 0u; i < 256u; i++)
    {
        uint32_t c = i;
        uint32_t k;

        for (k = 0u; k < 8u; k++)
        {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        s_table[i] = c;
    }
    s_tableReady = 1u;
}

uint32_t crc32_init_value(void)
{
    return 0xFFFFFFFFu;
}

uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    crc32_init();
    for (i = 0u; i < len; i++)
    {
        crc = s_table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc;
}

uint32_t crc32_final(uint32_t crc)
{
    return crc ^ 0xFFFFFFFFu;
}

uint32_t crc32_buf(const uint8_t *data, uint32_t len)
{
    return crc32_final(crc32_update(crc32_init_value(), data, len));
}
