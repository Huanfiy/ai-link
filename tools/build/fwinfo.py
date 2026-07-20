#!/usr/bin/env python3
"""Patch / verify the .fw_info metadata header of ailink images.

The 64-byte header sits at image base + 0x200 (layout: docs/design/ota.md,
C mirror: applications/ota/fw_info.h). The linked image carries a placeholder
with only the compile-time identity fields; `patch` fills in image size, the
two CRC32s, `git describe` version and build time, in both the .bin and the
ELF (so OpenOCD/dfu-util .bin flashing and VSCode ELF-based debug-load all
carry valid metadata). Wired into rtconfig.py POST_ACTION; standalone:

    python3 tools/build/fwinfo.py patch --elf build/ailink.elf build/ailink.bin
    python3 tools/build/fwinfo.py verify build/ailink.bin
"""

import argparse
import struct
import subprocess
import sys
import time
import zlib
from datetime import datetime, timezone
from pathlib import Path

FW_INFO_OFFSET = 0x200
FW_INFO_SIZE = 64
FW_INFO_MAGIC = 0x57464C41  # "ALFW" little-endian
FW_INFO_HDR_VER = 1
FW_INFO_BOARD_ID = 0x0A17F446
FW_VERSION_LEN = 32

# <IHHIIII = magic, hdr_ver, hdr_size, board_id, image_size, image_crc32,
# build_time; then fw_version[32], reserved u32, header_crc32 u32.
_FIXED_FMT = '<IHHIIII'


def fail(msg, hint=None):
    print(f'[error] {msg}', file=sys.stderr)
    if hint:
        print(f'[hint] {hint}', file=sys.stderr)
    sys.exit(1)


def image_crc32(image, size):
    """CRC32 over the whole image minus the 64-byte header itself."""
    crc = zlib.crc32(image[:FW_INFO_OFFSET])
    return zlib.crc32(image[FW_INFO_OFFSET + FW_INFO_SIZE:size], crc) & 0xFFFFFFFF


def parse_header(blob):
    fields = struct.unpack_from(_FIXED_FMT, blob)
    fixed_len = struct.calcsize(_FIXED_FMT)
    version = blob[fixed_len:fixed_len + FW_VERSION_LEN].split(b'\0', 1)[0].decode('ascii', 'replace')
    header_crc = struct.unpack_from('<I', blob, 60)[0]
    return {
        'magic': fields[0], 'hdr_ver': fields[1], 'hdr_size': fields[2],
        'board_id': fields[3], 'image_size': fields[4], 'image_crc32': fields[5],
        'build_time': fields[6], 'fw_version': version, 'header_crc32': header_crc,
    }


def git_describe():
    try:
        out = subprocess.run(
            ['git', 'describe', '--tags', '--dirty', '--always'],
            capture_output=True, text=True, timeout=10)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    except OSError:
        pass
    return 'unknown'


def build_header(image_size, img_crc, version, build_time):
    header = struct.pack(_FIXED_FMT, FW_INFO_MAGIC, FW_INFO_HDR_VER,
                         FW_INFO_SIZE, FW_INFO_BOARD_ID,
                         image_size, img_crc, build_time)
    header += version.encode('ascii', 'replace')[:FW_VERSION_LEN - 1].ljust(FW_VERSION_LEN, b'\0')
    header += struct.pack('<I', 0)  # reserved
    header += struct.pack('<I', zlib.crc32(header) & 0xFFFFFFFF)
    assert len(header) == FW_INFO_SIZE
    return header


def check_placeholder(blob, where):
    """The linked image must already carry the compile-time identity fields."""
    hdr = parse_header(blob)
    if hdr['magic'] != FW_INFO_MAGIC:
        fail(f'no .fw_info magic at image base + 0x{FW_INFO_OFFSET:X} in {where}',
             'is applications/ota/fw_info.c built and .fw_info placed by link.lds?')
    if hdr['hdr_ver'] != FW_INFO_HDR_VER or hdr['hdr_size'] != FW_INFO_SIZE:
        fail(f'unsupported .fw_info header {hdr["hdr_ver"]}/{hdr["hdr_size"]} in {where}')


def elf_fw_info_offset(elf_bytes):
    """File offset of the .fw_info header inside an ELF32-LE image.

    The header is an input section merged into .text, so instead of a section
    name lookup, resolve (lowest allocated PROGBITS address) + 0x200 back to a
    file offset through the section that contains it.
    """
    if elf_bytes[:4] != b'\x7fELF' or elf_bytes[4] != 1 or elf_bytes[5] != 1:
        fail('not an ELF32 little-endian file')
    e_shoff, = struct.unpack_from('<I', elf_bytes, 0x20)
    e_shentsize, e_shnum = struct.unpack_from('<HH', elf_bytes, 0x2E)

    SHT_PROGBITS, SHF_ALLOC = 1, 0x2
    sections = []
    for i in range(e_shnum):
        sh = struct.unpack_from('<10I', elf_bytes, e_shoff + i * e_shentsize)
        _, sh_type, sh_flags, sh_addr, sh_offset, sh_size = sh[:6]
        if sh_type == SHT_PROGBITS and sh_flags & SHF_ALLOC and sh_size:
            sections.append((sh_addr, sh_offset, sh_size))
    if not sections:
        fail('no allocated PROGBITS sections in ELF')

    target = min(addr for addr, _, _ in sections) + FW_INFO_OFFSET
    for sh_addr, sh_offset, sh_size in sections:
        if sh_addr <= target and target + FW_INFO_SIZE <= sh_addr + sh_size:
            return sh_offset + (target - sh_addr)
    fail(f'no ELF section covers image base + 0x{FW_INFO_OFFSET:X}')


def cmd_patch(args):
    image = bytearray(args.bin.read_bytes())
    if len(image) < FW_INFO_OFFSET + FW_INFO_SIZE:
        fail(f'{args.bin}: image too small ({len(image)} B)')
    check_placeholder(image[FW_INFO_OFFSET:FW_INFO_OFFSET + FW_INFO_SIZE], str(args.bin))

    version = git_describe()
    header = build_header(len(image), image_crc32(image, len(image)),
                          version, int(time.time()))
    image[FW_INFO_OFFSET:FW_INFO_OFFSET + FW_INFO_SIZE] = header
    args.bin.write_bytes(image)

    patched = [str(args.bin)]
    if args.elf:
        elf = bytearray(args.elf.read_bytes())
        off = elf_fw_info_offset(elf)
        check_placeholder(elf[off:off + FW_INFO_SIZE], str(args.elf))
        elf[off:off + FW_INFO_SIZE] = header
        args.elf.write_bytes(elf)
        patched.append(str(args.elf))

    print(f'[fwinfo] {version}, {len(image)} B, '
          f'crc 0x{parse_header(header)["image_crc32"]:08X} -> {", ".join(patched)}')


def cmd_verify(args):
    image = args.bin.read_bytes()
    if len(image) < FW_INFO_OFFSET + FW_INFO_SIZE:
        fail(f'{args.bin}: image too small ({len(image)} B)')
    blob = image[FW_INFO_OFFSET:FW_INFO_OFFSET + FW_INFO_SIZE]
    hdr = parse_header(blob)

    if hdr['magic'] != FW_INFO_MAGIC:
        fail(f'bad magic 0x{hdr["magic"]:08X}')
    if hdr['hdr_ver'] != FW_INFO_HDR_VER or hdr['hdr_size'] != FW_INFO_SIZE:
        fail(f'unsupported header version/size {hdr["hdr_ver"]}/{hdr["hdr_size"]}')
    if zlib.crc32(blob[:60]) & 0xFFFFFFFF != hdr['header_crc32']:
        fail('header CRC mismatch (unpatched placeholder or corrupt image)')
    if hdr['board_id'] != FW_INFO_BOARD_ID:
        fail(f'board_id 0x{hdr["board_id"]:08X} != 0x{FW_INFO_BOARD_ID:08X}')
    if hdr['image_size'] != len(image):
        fail(f'image_size {hdr["image_size"]} != file size {len(image)}')
    crc = image_crc32(image, hdr['image_size'])
    if crc != hdr['image_crc32']:
        fail(f'image CRC 0x{crc:08X} != header 0x{hdr["image_crc32"]:08X}')

    built = datetime.fromtimestamp(hdr['build_time'], timezone.utc)
    print(f'[fwinfo] {args.bin}: valid')
    print(f'  version    {hdr["fw_version"]}')
    print(f'  built      {built:%Y-%m-%d %H:%M:%S} UTC')
    print(f'  board_id   0x{hdr["board_id"]:08X}')
    print(f'  image      {hdr["image_size"]} B, crc 0x{hdr["image_crc32"]:08X}')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='cmd', required=True)

    p_patch = sub.add_parser('patch', help='fill size/CRC/version into built artifacts')
    p_patch.add_argument('bin', type=Path, help='raw binary image (patched in place)')
    p_patch.add_argument('--elf', type=Path,
                         help='matching ELF to patch with the same header')

    p_verify = sub.add_parser('verify', help='validate a patched .bin')
    p_verify.add_argument('bin', type=Path)

    args = parser.parse_args()
    for path in (args.bin, getattr(args, 'elf', None)):
        if path and not path.is_file():
            fail(f'file not found: {path}')
    cmd_patch(args) if args.cmd == 'patch' else cmd_verify(args)


if __name__ == '__main__':
    main()
