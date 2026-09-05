/*
 * ota_ep0.c - EP0 vendor requests for firmware upgrade (docs/design/ota.md).
 *
 * Claims 0x63/0x64 on the usb_bridge vendor extension slot, next to the
 * GPIO bank's 0x60-0x62 and outside the FTDI SIO_* code space:
 *
 *   0x63 OTA_REBOOT  (OUT): wValue 0 = plain reset; 1 = write the BKP0R
 *        trampoline magic first, so the bootloader forwards the next boot
 *        into ROM DFU. Reset is deferred ~100 ms to a one-shot timer so the
 *        EP0 status stage completes before the device drops off the bus.
 *   0x64 OTA_VERSION (IN): fw_version[32] + build_time + board_id + hdr_ver
 *        + image_crc32, read from the running image's own .fw_info header;
 *        the host tool compares this against the freshly flashed .bin.
 *
 * The handler runs in USB ISR context: it only stages state and arms the
 * timer. The timer callback runs in tick ISR context (hard timer; the BSP
 * builds without RT_USING_TIMER_SOFT), which suits both actions taken
 * there: backup-register writes and NVIC_SystemReset are ISR-safe.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtthread.h>
#include <board.h>

#include "usbd_core.h"

#include "usb_bridge.h"
#include "fw_info.h"

#define OTA_REQ_REBOOT  0x63U
#define OTA_REQ_VERSION 0x64U

#define OTA_REBOOT_DELAY_MS 100 /* let the EP0 status stage finish first */

#define DFU_TRAMPOLINE_MAGIC 0x5AFEB007UL

/* 0x64 reply: version string + identity words, packed little-endian */
struct ota_version_reply {
    char fw_version[FW_INFO_VER_LEN];
    uint32_t build_time;
    uint32_t board_id;
    uint16_t hdr_ver;
    uint16_t reserved;
    uint32_t image_crc32;
};

static struct rt_timer ota_reboot_timer;
static volatile uint8_t ota_want_dfu;

static void ota_reboot_timeout(void *param)
{
    (void)param;

    if (ota_want_dfu) {
        /* Arm the bootloader's DFU trampoline: BKP0R survives the system
         * reset (backup domain), gets consumed by boot on the way up. */
        __HAL_RCC_PWR_CLK_ENABLE();
        HAL_PWR_EnableBkUpAccess();
        RTC->BKP0R = DFU_TRAMPOLINE_MAGIC;
        HAL_PWR_DisableBkUpAccess();
    }
    rt_hw_cpu_reset();
}

static int ota_vendor_request_handler(uint8_t busid, struct usb_setup_packet *setup,
                                      uint8_t **data, uint32_t *len)
{
    (void)busid;

    switch (setup->bRequest) {
    case OTA_REQ_REBOOT:
        if (setup->wValue > 1) {
            return -1;
        }
        ota_want_dfu = (setup->wValue == 1);
        if (rt_timer_start(&ota_reboot_timer) != RT_EOK)
        {
            return -1;
        }
        bridge_product_prepare_reboot();
        return 0;

    case OTA_REQ_VERSION: {
        struct ota_version_reply *reply = (struct ota_version_reply *)*data;
        const struct fw_info *info = fw_info_get();

        rt_memcpy(reply->fw_version, info->fw_version, FW_INFO_VER_LEN);
        reply->build_time = info->build_time;
        reply->board_id = info->board_id;
        reply->hdr_ver = info->hdr_ver;
        reply->reserved = 0;
        reply->image_crc32 = info->image_crc32;

        *len = (setup->wLength < sizeof(*reply)) ? setup->wLength : sizeof(*reply);
        return 0;
    }

    default:
        return -1; /* not ours, keep the dispatch chain going */
    }
}

static int ota_ep0_init(void)
{
    rt_err_t err;

    /* one-shot; restartable if the host retries the request */
    rt_timer_init(&ota_reboot_timer, "otarst", ota_reboot_timeout, RT_NULL,
                  rt_tick_from_millisecond(OTA_REBOOT_DELAY_MS),
                  RT_TIMER_FLAG_ONE_SHOT);

    err = usb_bridge_register_vendor_ext(ota_vendor_request_handler);
    if (err != RT_EOK) {
        rt_kprintf("[ota] vendor ext slot taken, EP0 OTA requests disabled\n");
        return err;
    }
    return RT_EOK;
}
INIT_COMPONENT_EXPORT(ota_ep0_init);
