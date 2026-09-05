/*
 * ailink_product.h - v0.4 product power and indicator hardware.
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** @brief Establish inactive outputs before peripheral initialisation. */
void ailink_product_hw_init(void);
/** @brief Enable the protected supply switches, keeping target pins high-Z. */
void ailink_target_power_start(void);
/** @brief Restore target pins after the supply turn-on interval. */
void ailink_target_power_ready(void);
/** @brief Make target signals high-Z before removing target power. */
void ailink_target_power_stop(void);
/** @brief Return whether the target signals may currently drive. */
bool ailink_target_is_ready(void);
/** @brief Update four active-low activity LEDs and the RGB normal-status colour. */
void ailink_product_leds_write(uint8_t activity, bool configured);
