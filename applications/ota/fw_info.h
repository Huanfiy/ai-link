/*
 * fw_info.h - firmware image metadata header at image base + 0x200.
 *
 * 64-byte layout frozen in docs/design/ota.md, shared by the application
 * (EP0 OTA_VERSION reply), the bootloader (in-place image validation, via
 * include path) and tools/build/fwinfo.py (Python mirror). Fields are
 * naturally aligned and add up to exactly 64 bytes, no packing needed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef FW_INFO_H
#define FW_INFO_H

#include <stdint.h>

#define FW_INFO_MAGIC    0x57464C41U /* "ALFW" little-endian */
#define FW_INFO_HDR_VER  1U
#define FW_INFO_BOARD_ID 0x0A17F446U /* distinct from f407 (0x0A17F407) to block cross-flashing */
#define FW_INFO_OFFSET   0x200U /* from image base, right after the 0x188 B vector table */
#define FW_INFO_VER_LEN  32U

struct fw_info {
    uint32_t magic;      /* FW_INFO_MAGIC */
    uint16_t hdr_ver;    /* FW_INFO_HDR_VER */
    uint16_t hdr_size;   /* sizeof(struct fw_info) */
    uint32_t board_id;   /* FW_INFO_BOARD_ID */
    uint32_t image_size; /* whole image bytes, vector table and this header included */
    uint32_t image_crc32; /* zlib CRC32 over [0, 0x200) + [0x240, image_size) */
    uint32_t build_time;  /* build Unix timestamp (UTC) */
    char fw_version[FW_INFO_VER_LEN]; /* git describe --tags --dirty --always, NUL padded */
    uint32_t reserved;
    uint32_t header_crc32; /* zlib CRC32 over the first 60 bytes of this struct */
};

/**
 * @brief Access the running image's own metadata (flash copy at base + 0x200).
 * @return Pointer to the in-flash header; fields are valid only on images
 *         patched by tools/build/fwinfo.py.
 */
const struct fw_info *fw_info_get(void);

#endif /* FW_INFO_H */
