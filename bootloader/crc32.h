/*
 * crc32.h - zlib-compatible CRC32 for in-place image validation.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef BOOT_CRC32_H
#define BOOT_CRC32_H

#include <stdint.h>

/** @brief Generate the 256-entry lookup table (call once before update). */
void crc32_init(void);

/**
 * @brief Continue a zlib-style CRC32 (start with crc = 0, chain the result).
 * @param crc  Previous return value, or 0 for the first chunk.
 * @param data Bytes to checksum.
 * @param len  Byte count.
 * @return Updated CRC32.
 */
uint32_t crc32_update(uint32_t crc, const void *data, uint32_t len);

#endif /* BOOT_CRC32_H */
