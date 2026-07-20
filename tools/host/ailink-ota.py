#!/usr/bin/env python3
"""Firmware upgrade for the ailink over its ROM DFU channel.

One command wraps the whole flow (docs/design/ota.md):

  1. verify the local .bin's .fw_info header (magic/board_id/CRC) to block
     foreign or truncated images before anything touches the device;
  2. send EP0 vendor request 0x63 wValue=1: the app arms the BKP0R
     trampoline and resets, the bootloader forwards into ROM DFU;
  3. wait for the ROM DFU device (0483:df11), run dfu-util against the app
     region (fixed -s 0x08010000 so the bootloader sectors stay untouched);
  4. wait for the bridge (1209:0010) to re-enumerate, query 0x64 and compare
     the reported version/CRC against the image just flashed.

The device is fully offline for the ~20-30 s upgrade window (both ttyACM
ports and CMSIS-DAP drop); close any sessions holding them first.

If an upgrade is interrupted (cable pull, power loss), the bootloader's CRC
check fails on next boot and lands the device back in ROM DFU: re-running
`flash` recovers it (the tool skips the trigger when it already sees DFU).

Requires: dfu-util >= 0.9 in PATH, pyusb; udev rules from 99-ailink.rules.

Usage:
  ailink-ota.py flash build/ailink.bin    # full upgrade cycle
  ailink-ota.py info                           # 0x64: running fw version
  ailink-ota.py verify build/ailink.bin   # local image check only
"""

import argparse
import struct
import subprocess
import sys
import time
import zlib

import usb.core
import usb.util

BRIDGE_VID, BRIDGE_PID = 0x1209, 0x0010
BRIDGE_PRODUCT = "ailink USB bridge"
DFU_VID, DFU_PID = 0x0483, 0xDF11

REQ_OTA_REBOOT = 0x63
REQ_OTA_VERSION = 0x64

APP_FLASH_ADDR = 0x08010000
APP_MAX_SIZE = 448 * 1024

FW_INFO_OFFSET = 0x200
FW_INFO_SIZE = 64
FW_INFO_MAGIC = 0x57464C41
FW_INFO_HDR_VER = 1
FW_INFO_BOARD_ID = 0x0A17F446
FW_VERSION_LEN = 32

# fw_version[32] + build_time u32 + board_id u32 + hdr_ver u16 + reserved u16
# + image_crc32 u32 (struct ota_version_reply, applications/ota/ota_ep0.c)
VERSION_REPLY_LEN = 48

REQTYPE_OUT = usb.util.build_request_type(
    usb.util.CTRL_OUT, usb.util.CTRL_TYPE_VENDOR, usb.util.CTRL_RECIPIENT_DEVICE)
REQTYPE_IN = usb.util.build_request_type(
    usb.util.CTRL_IN, usb.util.CTRL_TYPE_VENDOR, usb.util.CTRL_RECIPIENT_DEVICE)


def fail(msg):
    sys.exit(f"error: {msg}")


def parse_fw_info(image):
    """Validate a local image's .fw_info header; returns the parsed fields."""
    if len(image) < FW_INFO_OFFSET + FW_INFO_SIZE:
        fail(f"image too small ({len(image)} B), no .fw_info header")
    blob = image[FW_INFO_OFFSET:FW_INFO_OFFSET + FW_INFO_SIZE]
    fixed_fmt = '<IHHIIII'
    magic, hdr_ver, hdr_size, board_id, image_size, image_crc, build_time = \
        struct.unpack_from(fixed_fmt, blob)
    ver_off = struct.calcsize(fixed_fmt)
    version = blob[ver_off:ver_off + FW_VERSION_LEN].split(b'\0', 1)[0].decode('ascii', 'replace')
    header_crc, = struct.unpack_from('<I', blob, 60)

    if magic != FW_INFO_MAGIC:
        fail("not an ailink image (bad .fw_info magic)")
    if hdr_ver != FW_INFO_HDR_VER or hdr_size != FW_INFO_SIZE:
        fail(f"unsupported .fw_info header {hdr_ver}/{hdr_size}")
    if zlib.crc32(blob[:60]) & 0xFFFFFFFF != header_crc:
        fail(".fw_info header CRC mismatch (unpatched or corrupt image)")
    if board_id != FW_INFO_BOARD_ID:
        fail(f"board_id 0x{board_id:08X} does not match this board")
    if image_size != len(image):
        fail(f".fw_info image_size {image_size} != file size {len(image)}")
    if image_size > APP_MAX_SIZE:
        fail(f"image {image_size} B exceeds app region ({APP_MAX_SIZE} B)")
    crc = zlib.crc32(image[:FW_INFO_OFFSET])
    crc = zlib.crc32(image[FW_INFO_OFFSET + FW_INFO_SIZE:], crc) & 0xFFFFFFFF
    if crc != image_crc:
        fail(f"image CRC 0x{crc:08X} != .fw_info 0x{image_crc:08X}")

    return {'version': version, 'build_time': build_time, 'crc': image_crc,
            'size': image_size}


def find_bridge():
    for dev in usb.core.find(idVendor=BRIDGE_VID, idProduct=BRIDGE_PID,
                             find_all=True):
        try:
            if usb.util.get_string(dev, dev.iProduct) == BRIDGE_PRODUCT:
                return dev
        except usb.core.USBError:
            continue  # e.g. a real FT2232H without read permission
    return None


def find_dfu():
    return usb.core.find(idVendor=DFU_VID, idProduct=DFU_PID)


def wait_for(what, probe, timeout_s):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        dev = probe()
        if dev is not None:
            return dev
        time.sleep(0.5)
    fail(f"timed out after {timeout_s}s waiting for {what}")


def query_version(dev):
    data = dev.ctrl_transfer(REQTYPE_IN, REQ_OTA_VERSION, 0, 0,
                             VERSION_REPLY_LEN, timeout=2000)
    if len(data) < VERSION_REPLY_LEN:
        fail(f"short OTA_VERSION reply ({len(data)} B)")
    version = bytes(data[:FW_VERSION_LEN]).split(b'\0', 1)[0].decode('ascii', 'replace')
    build_time, board_id, hdr_ver, _, image_crc = struct.unpack_from(
        '<IIHHI', bytes(data), FW_VERSION_LEN)
    return {'version': version, 'build_time': build_time, 'board_id': board_id,
            'hdr_ver': hdr_ver, 'crc': image_crc}


def run_dfu_util(image_path):
    cmd = ['dfu-util', '-a', '0', '-s', f'0x{APP_FLASH_ADDR:08X}:leave',
           '-D', str(image_path)]
    print(f"[ota] {' '.join(cmd)}")
    result = subprocess.run(cmd)
    if result.returncode != 0:
        fail(f"dfu-util exited {result.returncode} "
             "(device stays in DFU mode; re-run to retry)")


def cmd_info(_args):
    dev = find_bridge()
    if dev is None:
        if find_dfu() is not None:
            fail("device is in DFU mode (recovery pending), no version to query")
        fail(f"no {BRIDGE_PRODUCT} found (cable / udev permissions?)")
    running = query_version(dev)
    built = time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(running['build_time']))
    print(f"running firmware: {running['version']}")
    print(f"  built    {built} UTC")
    print(f"  board_id 0x{running['board_id']:08X}")
    print(f"  crc      0x{running['crc']:08X}")


def cmd_verify(args):
    with open(args.image, 'rb') as f:
        info = parse_fw_info(f.read())
    built = time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(info['build_time']))
    print(f"{args.image}: valid ailink image")
    print(f"  version {info['version']}, built {built} UTC, "
          f"{info['size']} B, crc 0x{info['crc']:08X}")


def cmd_flash(args):
    with open(args.image, 'rb') as f:
        image = f.read()
    new = parse_fw_info(image)
    print(f"[ota] image ok: {new['version']}, {new['size']} B, "
          f"crc 0x{new['crc']:08X}")

    # Interrupted-upgrade recovery: if the device already sits in ROM DFU
    # (bootloader fallback after a truncated flash), skip the 0x63 trigger.
    dfu = find_dfu()
    if dfu is not None:
        print("[ota] device already in DFU mode, skipping reboot trigger")
    else:
        bridge = find_bridge()
        if bridge is None:
            fail(f"no {BRIDGE_PRODUCT} and no DFU device found")
        running = query_version(bridge)
        print(f"[ota] running: {running['version']} "
              f"(crc 0x{running['crc']:08X})")
        if running['crc'] == new['crc'] and not args.force:
            print("[ota] device already runs this exact image, nothing to do "
                  "(--force to reflash)")
            return
        print("[ota] triggering reboot into ROM DFU (device drops offline "
              "for the whole upgrade)...")
        bridge.ctrl_transfer(REQTYPE_OUT, REQ_OTA_REBOOT, 1, 0, timeout=2000)
        usb.util.dispose_resources(bridge)
        wait_for("ROM DFU device (0483:df11)", find_dfu, args.timeout)
        time.sleep(0.5)  # let udev apply permissions before dfu-util opens it

    run_dfu_util(args.image)

    print("[ota] waiting for the bridge to re-enumerate...")
    time.sleep(1.0)
    bridge = wait_for(f"{BRIDGE_PRODUCT} (1209:0010)", find_bridge, args.timeout)
    time.sleep(0.5)

    running = query_version(bridge)
    if running['crc'] != new['crc']:
        fail(f"version check failed: device reports {running['version']} "
             f"(crc 0x{running['crc']:08X}), expected {new['version']} "
             f"(crc 0x{new['crc']:08X})")
    print(f"[ota] success: device now runs {running['version']} "
          f"(crc 0x{running['crc']:08X})")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    sub = parser.add_subparsers(dest='cmd', required=True)

    p_flash = sub.add_parser('flash', help='full upgrade: verify, trigger, dfu-util, compare')
    p_flash.add_argument('image', help='fwinfo-patched .bin (build/ailink.bin)')
    p_flash.add_argument('--force', action='store_true',
                         help='reflash even if the device reports the same CRC')
    p_flash.add_argument('--timeout', type=int, default=30,
                         help='seconds to wait for each enumeration (default 30)')
    p_flash.set_defaults(func=cmd_flash)

    p_info = sub.add_parser('info', help='query the running firmware version (0x64)')
    p_info.set_defaults(func=cmd_info)

    p_verify = sub.add_parser('verify', help='validate a local image, no device needed')
    p_verify.add_argument('image')
    p_verify.set_defaults(func=cmd_verify)

    args = parser.parse_args()
    args.func(args)


if __name__ == '__main__':
    main()
