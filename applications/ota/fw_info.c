/*
 * fw_info.c - link-time placeholder for the .fw_info metadata header.
 *
 * board/linker_scripts/link.lds pins the .fw_info input section at image
 * base + 0x200. Only compile-time identity fields are set here; size, CRCs,
 * version and build time stay zero until tools/build/fwinfo.py patches the
 * built artifacts, so an unpatched image fails the bootloader's header CRC
 * check by construction instead of booting with bogus metadata.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fw_info.h"

_Static_assert(sizeof(struct fw_info) == 64, "fw_info layout must be exactly 64 bytes");

static const struct fw_info fw_info_self __attribute__((used, section(".fw_info"))) = {
    .magic    = FW_INFO_MAGIC,
    .hdr_ver  = FW_INFO_HDR_VER,
    .hdr_size = (uint16_t)sizeof(struct fw_info),
    .board_id = FW_INFO_BOARD_ID,
};

const struct fw_info *fw_info_get(void)
{
    return &fw_info_self;
}
