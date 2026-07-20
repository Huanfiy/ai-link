/*
 * usbd_fs_port.c - Board-level FIFO partition for the OTG_FS device core.
 *
 * The CherryUSB dwc2 ST glue queries this hook when CONFIG_USB_DWC2_CUSTOM_FIFO
 * is defined in usb_config.h, replacing its default 4x64B partition.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <stdint.h>

#include "port/dwc2/usb_dwc2_param.h"

/*
 * OTG_FS has 320 words (1.25KB) of FIFO RAM. F446 exposes EP0 + EP1..EP5,
 * all six are in use (docs/design/usb-bridge.md). Partition (32-bit words):
 *   RX (shared)   128 -> 512B, absorbs bulk OUT bursts on both CDC channels
 *   EP0 TX         16 ->  64B, control IN
 *   EP1 TX         48 -> 192B, CDC ACM 0 bulk IN (data)
 *   EP2 TX         48 -> 192B, CDC ACM 1 bulk IN (data)
 *   EP3 TX         48 -> 192B, CMSIS-DAP v2 bulk IN
 *   EP4 TX         16 ->  64B, CDC ACM 0 notify IN (idle)
 *   EP5 TX         16 ->  64B, CDC ACM 1 notify IN (idle)
 * Total: 128 + 16 + 48 + 48 + 48 + 16 + 16 = 320 words.
 */
void dwc2_get_user_fifo_config(uint32_t reg_base, struct usb_dwc2_user_fifo_config *config)
{
    (void)reg_base; /* single OTG_FS instance on this board */

    config->device_rx_fifo_size = 128;

    for (uint8_t i = 0; i < MAX_EPS_CHANNELS; i++) {
        config->device_tx_fifo_size[i] = 0;
    }
    config->device_tx_fifo_size[0] = 16;
    config->device_tx_fifo_size[1] = 48;
    config->device_tx_fifo_size[2] = 48;
    config->device_tx_fifo_size[3] = 48;
    config->device_tx_fifo_size[4] = 16;
    config->device_tx_fifo_size[5] = 16;
}
