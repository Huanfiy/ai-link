/*
 * bridge_gpio.c - Host-controlled GPIO bank over EP0 vendor requests.
 *
 * Requests 0x60-0x62 keep the ailink-f407 protocol (bit mask + direction +
 * level in wValue); only the pin backing changed: the LQFP64 F446 has no
 * PE port, so the 8 logical bank bits map onto scattered pins via a table
 * instead of one whole-port register:
 *
 *   bit 0..7 = PB0, PB1, PB2, PB8, PB9, PB10, PA1, PA8
 *
 * Runs in USB ISR context: rt_pin_* on STM32 is register-level (microseconds).
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <rtdevice.h>
#include <board.h>
#include <drv_gpio.h>

#include "usbd_core.h"

#include "usb_bridge.h"

#define GPIO_PIN_COUNT 8U

/* logical bank bit -> physical pin (see header comment) */
static const rt_base_t gpio_pin_map[GPIO_PIN_COUNT] = {
    GET_PIN(B, 0), GET_PIN(B, 1), GET_PIN(B, 2), GET_PIN(B, 8),
    GET_PIN(B, 9), GET_PIN(B, 10), GET_PIN(A, 1), GET_PIN(A, 8),
};

/* bit set = configured as output; inputs come up floating */
static uint8_t gpio_dir_mask;

int bridge_gpio_request_handler(struct usb_setup_packet *setup,
                                uint8_t **data, uint32_t *len)
{
    switch (setup->bRequest) {
    case USB_BRIDGE_GPIO_REQ_CONFIG: {
        /* wValue: low byte = pin mask, high byte = direction (1 = output) */
        uint8_t mask = setup->wValue & 0xFF;
        uint8_t dir = (setup->wValue >> 8) & 0xFF;

        for (uint8_t i = 0; i < GPIO_PIN_COUNT; i++) {
            if (!(mask & (1U << i))) {
                continue;
            }
            if (dir & (1U << i)) {
                rt_pin_mode(gpio_pin_map[i], PIN_MODE_OUTPUT);
                gpio_dir_mask |= (1U << i);
            } else {
                rt_pin_mode(gpio_pin_map[i], PIN_MODE_INPUT);
                gpio_dir_mask &= (uint8_t)~(1U << i);
            }
        }
        return 0;
    }

    case USB_BRIDGE_GPIO_REQ_WRITE: {
        /* wValue: low byte = pin mask, high byte = levels */
        uint8_t mask = setup->wValue & 0xFF;
        uint8_t value = (setup->wValue >> 8) & 0xFF;

        if (mask & (uint8_t)~gpio_dir_mask) {
            return -1; /* refuse writing pins not configured as output */
        }
        for (uint8_t i = 0; i < GPIO_PIN_COUNT; i++) {
            if (mask & (1U << i)) {
                rt_pin_write(gpio_pin_map[i], (value & (1U << i)) ? PIN_HIGH : PIN_LOW);
            }
        }
        return 0;
    }

    case USB_BRIDGE_GPIO_REQ_READ: {
        uint8_t levels = 0;

        for (uint8_t i = 0; i < GPIO_PIN_COUNT; i++) {
            if (rt_pin_read(gpio_pin_map[i]) == PIN_HIGH) {
                levels |= (1U << i);
            }
        }
        (*data)[0] = levels;
        (*data)[1] = gpio_dir_mask;
        *len = (setup->wLength < 2) ? setup->wLength : 2;
        return 0;
    }

    default:
        return -1;
    }
}
