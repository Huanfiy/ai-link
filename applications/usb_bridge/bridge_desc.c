/*
 * bridge_desc.c - USB composite descriptors and device bring-up.
 *
 * Dual CDC ACM (IAD) + CMSIS-DAP v2 vendor interface on VID/PID 1209:0010
 * (pid.codes test space). Linux binds cdc_acm to both serial functions,
 * Windows 10+ binds usbser via the IADs; the DAP interface carries a
 * "CMSIS-DAP" iInterface string (pyOCD/OpenOCD v2 detection) and gets
 * WinUSB auto-bound on Windows through BOS + MS OS 2.0 descriptors.
 * The serial number string is derived from the MCU 96-bit UID so
 * /dev/serial/by-id paths stay stable across boards.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <board.h>

#include "usbd_core.h"
#include "usbd_cdc_acm.h"
#include "usb_cdc.h"

#include "usb_bridge.h"

#define USBD_VID       0x1209 /* pid.codes open-source VID */
#define USBD_PID       0x0010 /* pid.codes test PID, not for general sale */
#define USBD_BCDDEVICE 0x0100
#define USBD_MAX_POWER 500    /* mA */
#define USBD_LANGID    0x0409 /* en-US */

/* 9 config + 2 * CDC ACM function (IAD 8 + 9+5+5+4+5 + 7 + 9 + 7+7)
 * + DAP (9 interface + 2 * 7 endpoint) */
#define USB_CONFIG_SIZE (9 + 2 * CDC_ACM_DESCRIPTOR_LEN + 9 + 7 + 7)

/* string index 4: pyOCD/OpenOCD detect CMSIS-DAP v2 by this substring */
#define USBD_STRING_DAP_INDEX 4

static const uint8_t device_descriptor[] = {
    /* USB 2.1 so hosts ask for the BOS descriptor (MS OS 2.0 lives there);
     * EF/02/01 = composite device with IADs */
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_1, 0xEF, 0x02, 0x01,
                               USBD_VID, USBD_PID, USBD_BCDDEVICE, 0x01)
};

static const uint8_t config_descriptor[] = {
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x05, 0x01,
                               USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    /* Interfaces 0+1: CDC ACM function 0, channel A (uart6) */
    CDC_ACM_DESCRIPTOR_INIT(USB_BRIDGE_INTF_CDC0_COMM, USB_BRIDGE_A_NOTIFY_EP,
                            USB_BRIDGE_A_OUT_EP, USB_BRIDGE_A_IN_EP,
                            USB_BRIDGE_BULK_MPS, 0x00),
    /* Interfaces 2+3: CDC ACM function 1, channel B (uart1) */
    CDC_ACM_DESCRIPTOR_INIT(USB_BRIDGE_INTF_CDC1_COMM, USB_BRIDGE_B_NOTIFY_EP,
                            USB_BRIDGE_B_OUT_EP, USB_BRIDGE_B_IN_EP,
                            USB_BRIDGE_BULK_MPS, 0x00),
    /* Interface 4: CMSIS-DAP v2 (vendor specific, bulk OUT then IN per spec) */
    USB_INTERFACE_DESCRIPTOR_INIT(USB_BRIDGE_INTF_DAP, 0x00, 0x02,
                                  0xFF, 0x00, 0x00, USBD_STRING_DAP_INDEX),
    USB_ENDPOINT_DESCRIPTOR_INIT(USB_BRIDGE_DAP_OUT_EP, USB_ENDPOINT_TYPE_BULK,
                                 USB_BRIDGE_BULK_MPS, 0x00),
    USB_ENDPOINT_DESCRIPTOR_INIT(USB_BRIDGE_DAP_IN_EP, USB_ENDPOINT_TYPE_BULK,
                                 USB_BRIDGE_BULK_MPS, 0x00),
};

/* ---------------- BOS + MS OS 2.0: WinUSB on the DAP interface ----------------
 * Composite layout: set header > configuration subset > function subset
 * (interface 4) > CompatibleID "WINUSB" + DeviceInterfaceGUIDs registry
 * property. CDC functions are excluded on purpose: usbser must keep them.
 */

#define USBD_WINUSB_VENDOR_CODE 0x20 /* outside 0x60-0x64 vendor commands */

#define WINUSB_SET_HEADER_SIZE        10
#define WINUSB_SUBSET_CONFIG_SIZE     8
#define WINUSB_SUBSET_FUNCTION_SIZE   8
#define WINUSB_FEATURE_COMPAT_ID_SIZE 20
#define WINUSB_FEATURE_GUIDS_SIZE     132 /* REG_MULTI_SZ DeviceInterfaceGUIDs */

#define WINUSB_FUNCTION_LEN (WINUSB_SUBSET_FUNCTION_SIZE + \
                             WINUSB_FEATURE_COMPAT_ID_SIZE + \
                             WINUSB_FEATURE_GUIDS_SIZE)
#define WINUSB_CONFIG_LEN (WINUSB_SUBSET_CONFIG_SIZE + WINUSB_FUNCTION_LEN)
#define WINUSB_DESC_SET_LEN (WINUSB_SET_HEADER_SIZE + WINUSB_CONFIG_LEN)

static const uint8_t msosv2_descriptor_set[] = {
    /* Microsoft OS 2.0 descriptor set header */
    WBVAL(WINUSB_SET_HEADER_SIZE),            /* wLength */
    WBVAL(WINUSB_SET_HEADER_DESCRIPTOR_TYPE), /* wDescriptorType */
    0x00, 0x00, 0x03, 0x06,                   /* dwWindowsVersion >= 8.1 */
    WBVAL(WINUSB_DESC_SET_LEN),               /* wTotalLength */
    /* Configuration subset header (index 0 = the only configuration) */
    WBVAL(WINUSB_SUBSET_CONFIG_SIZE),               /* wLength */
    WBVAL(WINUSB_SUBSET_HEADER_CONFIGURATION_TYPE), /* wDescriptorType */
    0x00,                                           /* bConfigurationValue (index) */
    0x00,                                           /* bReserved */
    WBVAL(WINUSB_CONFIG_LEN),                       /* wTotalLength */
    /* Function subset header: DAP interface only */
    WBVAL(WINUSB_SUBSET_FUNCTION_SIZE),        /* wLength */
    WBVAL(WINUSB_SUBSET_HEADER_FUNCTION_TYPE), /* wDescriptorType */
    USB_BRIDGE_INTF_DAP,                       /* bFirstInterface */
    0x00,                                      /* bReserved */
    WBVAL(WINUSB_FUNCTION_LEN),                /* wSubsetLength */
    /* CompatibleID feature: bind WinUSB */
    WBVAL(WINUSB_FEATURE_COMPAT_ID_SIZE),     /* wLength */
    WBVAL(WINUSB_FEATURE_COMPATIBLE_ID_TYPE), /* wDescriptorType */
    'W', 'I', 'N', 'U', 'S', 'B', 0, 0,       /* CompatibleID */
    0, 0, 0, 0, 0, 0, 0, 0,                   /* SubCompatibleID */
    /* Registry property: DeviceInterfaceGUIDs (REG_MULTI_SZ) with the
     * CMSIS-DAP v2 GUID {CDB3B5AD-293B-4663-AA36-1AAE46463776} */
    WBVAL(WINUSB_FEATURE_GUIDS_SIZE),          /* wLength */
    WBVAL(WINUSB_FEATURE_REG_PROPERTY_TYPE),   /* wDescriptorType */
    WBVAL(WINUSB_PROP_DATA_TYPE_REG_MULTI_SZ), /* wPropertyDataType */
    WBVAL(42),                                 /* wPropertyNameLength */
    'D', 0, 'e', 0, 'v', 0, 'i', 0, 'c', 0, 'e', 0,
    'I', 0, 'n', 0, 't', 0, 'e', 0, 'r', 0, 'f', 0, 'a', 0, 'c', 0, 'e', 0,
    'G', 0, 'U', 0, 'I', 0, 'D', 0, 's', 0, 0, 0,
    WBVAL(80), /* wPropertyDataLength */
    '{', 0,
    'C', 0, 'D', 0, 'B', 0, '3', 0, 'B', 0, '5', 0, 'A', 0, 'D', 0, '-', 0,
    '2', 0, '9', 0, '3', 0, 'B', 0, '-', 0,
    '4', 0, '6', 0, '6', 0, '3', 0, '-', 0,
    'A', 0, 'A', 0, '3', 0, '6', 0, '-', 0,
    '1', 0, 'A', 0, 'A', 0, 'E', 0, '4', 0, '6', 0, '4', 0, '6', 0,
    '3', 0, '7', 0, '7', 0, '6', 0,
    '}', 0, 0, 0, 0, 0,
};

_Static_assert(sizeof(msosv2_descriptor_set) == WINUSB_DESC_SET_LEN,
               "MS OS 2.0 descriptor set length mismatch");

static struct usb_msosv2_descriptor msosv2_desc = {
    .vendor_code = USBD_WINUSB_VENDOR_CODE,
    .compat_id = msosv2_descriptor_set,
    .compat_id_len = WINUSB_DESC_SET_LEN,
};

#define USBD_WINUSB_DESC_LEN  28
#define USBD_BOS_WTOTALLENGTH (5 + USBD_WINUSB_DESC_LEN)

static const uint8_t bos_descriptor_set[] = {
    0x05,                         /* bLength */
    0x0F,                         /* bDescriptorType: BOS */
    WBVAL(USBD_BOS_WTOTALLENGTH), /* wTotalLength */
    0x01,                         /* bNumDeviceCaps */
    /* Microsoft OS 2.0 platform capability */
    USBD_WINUSB_DESC_LEN,           /* bLength */
    0x10,                           /* bDescriptorType: DEVICE CAPABILITY */
    USB_DEVICE_CAPABILITY_PLATFORM, /* bDevCapabilityType */
    0x00,                           /* bReserved */
    0xDF, 0x60, 0xDD, 0xD8,         /* MS OS 2.0 PlatformCapabilityUUID */
    0x89, 0x45, 0xC7, 0x4C,
    0x9C, 0xD2, 0x65, 0x9D,
    0x9E, 0x64, 0x8A, 0x9F,
    0x00, 0x00, 0x03, 0x06,      /* dwWindowsVersion >= 8.1 */
    WBVAL(WINUSB_DESC_SET_LEN),  /* wMSOSDescriptorSetTotalLength */
    USBD_WINUSB_VENDOR_CODE,     /* bMS_VendorCode */
    0x00,                        /* bAltEnumCode */
};

_Static_assert(sizeof(bos_descriptor_set) == USBD_BOS_WTOTALLENGTH,
               "BOS descriptor length mismatch");

static struct usb_bos_descriptor bos_desc = {
    .string = bos_descriptor_set,
    .string_len = USBD_BOS_WTOTALLENGTH,
};

/* 96-bit UID printed as 24 hex chars + NUL */
static char serial_string[25];

static const uint8_t *device_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return device_descriptor;
}

static const uint8_t *config_descriptor_callback(uint8_t speed)
{
    (void)speed;
    return config_descriptor;
}

static const char *string_descriptor_callback(uint8_t speed, uint8_t index)
{
    static const char langid[2] = { USBD_LANGID & 0xFF, USBD_LANGID >> 8 };

    (void)speed;

    switch (index) {
    case USB_STRING_LANGID_INDEX:
        return langid;
    case USB_STRING_MFC_INDEX:
        return "ailink";
    case USB_STRING_PRODUCT_INDEX:
        return "ailink USB bridge";
    case USB_STRING_SERIAL_INDEX:
        return serial_string;
    case USBD_STRING_DAP_INDEX:
        return "ailink CMSIS-DAP v2";
    default:
        return RT_NULL;
    }
}

static const struct usb_descriptor bridge_descriptor = {
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback,
    .msosv2_descriptor = &msosv2_desc,
    .bos_descriptor = &bos_desc,
};

/* CDC comm interfaces get the class handler from usbd_cdc_acm_init_intf;
 * the vendor dispatcher (GPIO + OTA extension) rides on intf0's vendor slot
 * since usbd_core tries every interface handler until one accepts. */
static struct usbd_interface intf_cdc0_comm;
static struct usbd_interface intf_cdc0_data;
static struct usbd_interface intf_cdc1_comm;
static struct usbd_interface intf_cdc1_data;
static struct usbd_interface intf_dap;

static void bridge_event_handler(uint8_t busid, uint8_t event)
{
    (void)busid;
    bridge_product_notify_event(event);
    bridge_pump_notify_event(event);
    bridge_dap_notify_event(event);
}

static void serial_string_from_uid(void)
{
    rt_snprintf(serial_string, sizeof(serial_string), "%08X%08X%08X",
                HAL_GetUIDw2(), HAL_GetUIDw1(), HAL_GetUIDw0());
}

static int usb_bridge_init(void)
{
    rt_err_t err;

    serial_string_from_uid();

    err = bridge_product_start();
    if (err != RT_EOK)
    {
        return err;
    }

    /* pump/DAP threads and UARTs must be live before the host configures us */
    err = bridge_pump_start();
    if (err != RT_EOK) {
        return err;
    }
    err = bridge_dap_start();
    if (err != RT_EOK) {
        return err;
    }

    usbd_desc_register(USB_BRIDGE_BUSID, &bridge_descriptor);

    usbd_add_interface(USB_BRIDGE_BUSID,
                       usbd_cdc_acm_init_intf(USB_BRIDGE_BUSID, &intf_cdc0_comm));
    usbd_add_interface(USB_BRIDGE_BUSID,
                       usbd_cdc_acm_init_intf(USB_BRIDGE_BUSID, &intf_cdc0_data));
    usbd_add_interface(USB_BRIDGE_BUSID,
                       usbd_cdc_acm_init_intf(USB_BRIDGE_BUSID, &intf_cdc1_comm));
    usbd_add_interface(USB_BRIDGE_BUSID,
                       usbd_cdc_acm_init_intf(USB_BRIDGE_BUSID, &intf_cdc1_data));
    usbd_add_interface(USB_BRIDGE_BUSID, &intf_dap);

    /* EP0 vendor requests (GPIO 0x60-0x62, OTA 0x63/0x64) are device-scoped;
     * hook the dispatcher after init_intf cleared the slot */
    intf_cdc0_comm.vendor_handler = bridge_vendor_request_handler;

    bridge_pump_register_endpoints();
    bridge_dap_register_endpoints();

    if (usbd_initialize(USB_BRIDGE_BUSID, USB_BRIDGE_OTG_FS_BASE,
                        bridge_event_handler) != 0) {
        rt_kprintf("[usb_bridge] usbd_initialize failed\n");
        return -RT_ERROR;
    }
    return RT_EOK;
}
INIT_APP_EXPORT(usb_bridge_init);
