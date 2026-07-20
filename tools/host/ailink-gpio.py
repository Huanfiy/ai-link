#!/usr/bin/env python3
"""Control the ailink USB bridge GPIO bank from the host.

Talks vendor requests 0x60-0x62 on EP0 of the composite device (1209:0010,
product string "ailink USB bridge"). Control transfers do not claim any
interface, so this tool coexists with cdc_acm and open ttyACM ports.

Bank bit -> pin mapping (see applications/usb_bridge/bridge_gpio.c):
  bit 0..7 = PB0, PB1, PB2, PB8, PB9, PB10, PA1, PA8

Usage:
  ailink-gpio.py dir <mask> <dir>      configure: bit=1 in <dir> -> output
  ailink-gpio.py set <mask> <value>    drive output pins
  ailink-gpio.py get                   read all pin levels
  ailink-gpio.py pulse <mask> --ms N   drive high then low for N ms

Masks/values accept decimal (255), hex (0xFF) or binary (0b11110000).
Examples:
  ailink-gpio.py dir 0xFF 0xFF         all 8 pins output
  ailink-gpio.py set 0x0F 0x05         bit0/bit2 high, bit1/bit3 low
  ailink-gpio.py get
"""

import argparse
import sys
import time

import usb.core
import usb.util

VID = 0x1209
PID = 0x0010
PRODUCT = "ailink USB bridge"

REQ_CONFIG = 0x60
REQ_WRITE = 0x61
REQ_READ = 0x62

REQTYPE_OUT = usb.util.build_request_type(
    usb.util.CTRL_OUT, usb.util.CTRL_TYPE_VENDOR, usb.util.CTRL_RECIPIENT_DEVICE)
REQTYPE_IN = usb.util.build_request_type(
    usb.util.CTRL_IN, usb.util.CTRL_TYPE_VENDOR, usb.util.CTRL_RECIPIENT_DEVICE)


def find_device():
    for dev in usb.core.find(idVendor=VID, idProduct=PID, find_all=True):
        try:
            if usb.util.get_string(dev, dev.iProduct) == PRODUCT:
                return dev
        except usb.core.USBError:
            continue  # real FT2232H without permission etc.
    sys.exit(f"no {PRODUCT} ({VID:04x}:{PID:04x}) found "
             "(check cable / udev permissions)")


def parse_byte(text: str) -> int:
    value = int(text, 0)
    if not 0 <= value <= 0xFF:
        raise argparse.ArgumentTypeError(f"{text}: out of 0..255")
    return value


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_dir = sub.add_parser("dir", help="configure pin directions")
    p_dir.add_argument("mask", type=parse_byte)
    p_dir.add_argument("dir", type=parse_byte)

    p_set = sub.add_parser("set", help="write output pins")
    p_set.add_argument("mask", type=parse_byte)
    p_set.add_argument("value", type=parse_byte)

    sub.add_parser("get", help="read pin levels")

    p_pulse = sub.add_parser("pulse", help="pulse pins high for --ms")
    p_pulse.add_argument("mask", type=parse_byte)
    p_pulse.add_argument("--ms", type=int, default=100)

    args = ap.parse_args()
    dev = find_device()

    if args.cmd == "dir":
        dev.ctrl_transfer(REQTYPE_OUT, REQ_CONFIG, (args.dir << 8) | args.mask, 0)
    elif args.cmd == "set":
        dev.ctrl_transfer(REQTYPE_OUT, REQ_WRITE, (args.value << 8) | args.mask, 0)
    elif args.cmd == "get":
        pin_names = ["PB0", "PB1", "PB2", "PB8", "PB9", "PB10", "PA1", "PA8"]
        data = dev.ctrl_transfer(REQTYPE_IN, REQ_READ, 0, 0, 2)
        levels, dirs = data[0], data[1]
        print(f"levels: 0x{levels:02X} 0b{levels:08b}")
        print(f"dirs  : 0x{dirs:02X} 0b{dirs:08b} (1=output)")
        for i in range(8):
            print(f"  bit{i} {pin_names[i]:>4}: {'out' if dirs >> i & 1 else 'in '} "
                  f"{'H' if levels >> i & 1 else 'L'}")
    elif args.cmd == "pulse":
        dev.ctrl_transfer(REQTYPE_OUT, REQ_CONFIG, (args.mask << 8) | args.mask, 0)
        dev.ctrl_transfer(REQTYPE_OUT, REQ_WRITE, (args.mask << 8) | args.mask, 0)
        time.sleep(args.ms / 1000)
        dev.ctrl_transfer(REQTYPE_OUT, REQ_WRITE, (0 << 8) | args.mask, 0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
