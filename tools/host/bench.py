#!/usr/bin/env python3
"""Loopback throughput / integrity bench for the ailink USB bridge.

Requires a TX-RX loopback jumper on the target UART (channel A: PC6-PC7,
channel B: PA9-PA10). The link has no flow control, so the writer is
window-limited against the reader: in-flight bytes never exceed --window,
keeping the device-side RX ring (8KB per channel) from overflowing.

Usage:
  bench.py PORT BAUD [-n BYTES] [--window BYTES]
  bench.py /dev/ttyACM1 4000000 -n 1000000
"""

import argparse
import os
import sys
import time

import serial


def bench(port: str, baud: int, total: int, window: int) -> int:
    payload = os.urandom(total)
    view = memoryview(payload)

    s = serial.Serial(port, baud, timeout=0, write_timeout=0)
    s.reset_input_buffer()

    tx = 0
    rx = bytearray()
    mismatch_at = -1
    t0 = time.time()
    deadline = t0 + max(30.0, total * 10.0 / baud * 3 + 5)

    while len(rx) < total and time.time() < deadline:
        if tx < total and (tx - len(rx)) < window:
            try:
                tx += s.write(view[tx:tx + min(4096, window - (tx - len(rx)))])
            except serial.SerialTimeoutException:
                pass
        chunk = s.read(65536)
        if chunk:
            base = len(rx)
            rx.extend(chunk)
            if mismatch_at < 0 and payload[base:base + len(chunk)] != chunk:
                mismatch_at = base
        else:
            time.sleep(0.0005)

    elapsed = time.time() - t0
    s.close()

    ok = (len(rx) == total) and (mismatch_at < 0) and bytes(rx) == payload
    rate = len(rx) / elapsed / 1000 if elapsed > 0 else 0
    line_max = baud / 10 / 1000  # 8N1: 10 bits per byte
    print(f"{port} @{baud}: {'OK' if ok else 'FAIL'} "
          f"rx={len(rx)}/{total} {rate:.1f}KB/s (line max {line_max:.0f}KB/s)"
          + (f" first-mismatch@{mismatch_at}" if mismatch_at >= 0 else ""))
    return 0 if ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("baud", type=int)
    ap.add_argument("-n", "--bytes", type=int, default=200000,
                    help="payload size (default 200000)")
    ap.add_argument("--window", type=int, default=2048,
                    help="max in-flight bytes (default 2048, must stay below "
                         "the device RX ring size)")
    args = ap.parse_args()
    return bench(args.port, args.baud, args.bytes, args.window)


if __name__ == "__main__":
    sys.exit(main())
