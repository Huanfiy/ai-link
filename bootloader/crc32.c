/*
 * crc32.c - table-driven zlib CRC32 (polynomial 0xEDB88320).
 *
 * The 1 KB table is generated once at startup into .bss rather than stored
 * as a ROM constant; at HSI 16 MHz this checks a full 448 KB app region in
 * well under a second, so boot latency stays negligible.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "crc32.h"

static uint32_t crc_table[256];

void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;

        for (uint32_t k = 0; k < 8; k++) {
            c = (c & 1U) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
        }
        crc_table[i] = c;
    }
}

uint32_t crc32_update(uint32_t crc, const void *data, uint32_t len)
{
    const uint8_t *p = (const uint8_t *)data;

    crc = ~crc;
    while (len--) {
        crc = crc_table[(crc ^ *p++) & 0xFFU] ^ (crc >> 8);
    }
    return ~crc;
}
