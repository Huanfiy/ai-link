#!/bin/sh
# install.sh - one-shot host setup for the ailink USB bridge.
#
# Installs 99-ailink.rules into /etc/udev/rules.d/ so that:
#   - 1209:0010 ("ailink USB bridge") is plugdev-accessible, letting
#     pyusb tools (ailink-gpio.py, ailink-ota.py) and pyOCD run without sudo;
#   - 0483:df11 (STM32 ROM DFU, the ailink-ota.py upgrade channel) is
#     plugdev-accessible so dfu-util runs without sudo.
#
# Usage: sudo ./install.sh

set -eu

cd "$(dirname "$0")"

if [ "$(id -u)" -ne 0 ]; then
    echo "must run as root: sudo ./install.sh" >&2
    exit 1
fi

install -m 0644 99-ailink.rules /etc/udev/rules.d/99-ailink.rules
udevadm control --reload

# Apply to an already-connected device.
udevadm trigger --subsystem-match=usb \
    --attr-match=idVendor=1209 --attr-match=idProduct=0010 || true

echo "installed: /etc/udev/rules.d/99-ailink.rules"
