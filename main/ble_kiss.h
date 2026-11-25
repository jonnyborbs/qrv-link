/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Jon Schulman
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * ble_kiss: BLE peripheral implementing the BLE-KISS API
 * (https://github.com/hessu/aprs-specs/blob/master/BLE-KISS-API.md).
 *
 * Service  00000001-ba2a-46c9-ae49-01b0961f68bb
 *   TX     00000002-...  app -> TNC, write / write without response
 *          descriptor 0x2901 "Write KISS data:XX": XX = capability bitmask (hex)
 *   RX     00000003-...  TNC -> app, notify
 *
 * The byte streams are passed through unchanged: KISS framing is end to end
 * between the app and the radio's TNC.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Called from the NimBLE host task with bytes the app wrote to TX. Keep it short. */
typedef void (*ble_kiss_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);
/* Called from the NimBLE host task when an app subscribes to RX (true) or
 * unsubscribes / disconnects (false). */
typedef void (*ble_kiss_session_cb_t)(bool active, void *ctx);

/*
 * Capability bits declared in the TX characteristic's User Description
 * descriptor (0x2901) as "Write KISS data:XX", a convention from Island Magic
 * (RadioMail / B.B. Link). Apps read it during discovery.
 */
#define BLE_KISS_CAP_QSY        0x01 /* SetHardware 0xEA / 0xEB frequency control */
#define BLE_KISS_CAP_BAUD_9600  0x02 /* SetHardware 0xF4 01: 9600 baud */

typedef struct {
    const char *device_name;
    uint8_t capabilities;      /* BLE_KISS_CAP_* bits */
    ble_kiss_rx_cb_t rx_cb;
    ble_kiss_session_cb_t session_cb;
    void *ctx;
} ble_kiss_config_t;

/* Initialise NimBLE, register the service and start advertising. NVS must be initialised. */
esp_err_t ble_kiss_start(const ble_kiss_config_t *config);

/*
 * Queue bytes for notification to the subscribed app. Never blocks; returns
 * the number of bytes accepted (less than len if the buffer is full) or 0 if
 * no app is subscribed.
 */
size_t ble_kiss_send(const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif
