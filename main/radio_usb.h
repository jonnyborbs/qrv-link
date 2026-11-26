/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Jon Schulman
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * radio_usb: USB host link to the Kenwood TM-D750's CDC-ACM serial port.
 *
 * Installs the ESP32-S3 USB host stack, waits for the radio to be plugged in,
 * opens its CDC-ACM interface and hands every received byte to a callback.
 * When the radio is unplugged the device is closed and the module goes back to
 * waiting, so replugging the cable reconnects automatically.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* JVCKENWOOD TM-D750, USB Full Speed. The main unit's port is a composite
 * device (CDC-ACM + USB Audio); the operating panel's USB-C port is CDC-ACM
 * only. Both carry CAT and KISS (menu 984 picks which port KISS uses). */
#define RADIO_USB_VID       0x2166
#define RADIO_USB_PID_MAIN  0x9030 /* main unit */
#define RADIO_USB_PID_PANEL 0x9032 /* operating (front) panel */
/* CDC-ACM communication interface; the data interface (1) is found by the driver. */
#define RADIO_USB_CDC_INTERFACE 0

/* Called from the USB driver task for every chunk received from the radio. Keep it short. */
typedef void (*radio_usb_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);
/* Called from the radio_usb task after the radio is opened (true) or closed (false). */
typedef void (*radio_usb_state_cb_t)(bool connected, void *ctx);

typedef struct {
    radio_usb_rx_cb_t rx_cb;
    radio_usb_state_cb_t state_cb;
    void *ctx;
} radio_usb_config_t;

/* Install the USB host stack and start waiting for the radio. Call once. */
esp_err_t radio_usb_start(const radio_usb_config_t *config);

/* True while the radio's CDC-ACM port is open. */
bool radio_usb_is_connected(void);

/*
 * Set the DTR line (RTS stays low). The TM-D750 only reads and answers its USB
 * serial port while DTR is high, and only after a DTR rising edge that arrives
 * once it is ready; the port opens with DTR low.
 */
esp_err_t radio_usb_set_dtr(bool dtr);

/* Send bytes to the radio. Returns ESP_ERR_INVALID_STATE if it is not connected. */
esp_err_t radio_usb_write(const uint8_t *data, size_t len, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
