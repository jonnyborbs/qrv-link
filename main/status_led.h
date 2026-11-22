/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Jon Schulman
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

/*
 * status_led: the DevKitC's addressable RGB LED as a packet activity light.
 *
 *   off    no app connected
 *   blue   an app is connected, idle
 *   green  a KISS data frame arrived from the radio (short flash)
 *   red    a KISS data frame was sent to the radio for transmission; lit for
 *          an estimate of TXDELAY plus the frame's airtime, since KISS gives no
 *          "transmission done" signal
 *
 * All calls are non-blocking and safe from any task; a small task owns the LED.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the LED task. Does nothing (and returns ESP_OK) if the LED is disabled in menuconfig. */
esp_err_t status_led_start(void);

/* An app is connected (blue while idle) or not (off). */
void status_led_set_connected(bool connected);

/* A data frame arrived from the radio: flash green. */
void status_led_rx(void);

/* A data frame went to the radio: red for `ms` milliseconds. */
void status_led_tx(uint32_t ms);

#ifdef __cplusplus
}
#endif
