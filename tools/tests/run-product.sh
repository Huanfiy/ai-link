#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
cd "$(dirname "$0")/../.."
test_dir="$(mktemp -d)"
trap 'rm -rf "$test_dir"' EXIT
for hz in 100 1000; do
    gcc -std=c11 -Wall -Wextra -Werror -fsanitize=undefined,address \
        -DTEST_TICK_HZ="$hz" -Itools/tests/product_stubs -Iboard/ports \
        -Iapplications/usb_bridge tools/tests/test_product.c \
        board/ports/ailink_product.c applications/usb_bridge/bridge_product.c \
        -o "$test_dir/product-$hz"
    "$test_dir/product-$hz"
done
