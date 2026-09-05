/* Product host-test USB boundary. SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
struct usb_setup_packet;
enum
{
    USBD_EVENT_RESET,
    USBD_EVENT_DISCONNECTED,
    USBD_EVENT_SUSPEND,
    USBD_EVENT_DEINIT,
    USBD_EVENT_CONFIGURED,
    USBD_EVENT_RESUME
};
bool usb_device_is_configured(uint8_t busid);
bool usb_device_is_suspend(uint8_t busid);
