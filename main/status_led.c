/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Jon Schulman
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "status_led.h"

#include "sdkconfig.h"

#if CONFIG_QRV_LINK_STATUS_LED

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "led_strip.h"

static const char *TAG = "status_led";

#define RX_FLASH_MS   150
#define SELFTEST_MS   250

typedef enum { EV_CONNECTED, EV_DISCONNECTED, EV_RX, EV_TX } led_event_t;

typedef struct {
    led_event_t type;
    uint32_t ms;
} led_msg_t;

typedef enum { COLOR_OFF, COLOR_BLUE, COLOR_GREEN, COLOR_RED } color_t;

static QueueHandle_t s_queue;
static led_strip_handle_t s_strip;

static void post(led_event_t type, uint32_t ms)
{
    if (s_queue) {
        const led_msg_t msg = { .type = type, .ms = ms };
        xQueueSend(s_queue, &msg, 0); /* never block the caller; drop if full */
    }
}

static const char *const COLOR_NAMES[] = { "off", "blue", "green", "red" };

static esp_err_t show(color_t color)
{
    const uint32_t b = CONFIG_QRV_LINK_STATUS_LED_BRIGHTNESS;
    const uint32_t idle = b / 2 ? b / 2 : 1; /* the steady idle colour a little dimmer */
    esp_err_t err = ESP_OK;
    switch (color) {
    case COLOR_BLUE:  err = led_strip_set_pixel(s_strip, 0, 0, 0, idle); break;
    case COLOR_GREEN: err = led_strip_set_pixel(s_strip, 0, 0, b, 0); break;
    case COLOR_RED:   err = led_strip_set_pixel(s_strip, 0, b, 0, 0); break;
    case COLOR_OFF:   return led_strip_clear(s_strip);
    }
    return err == ESP_OK ? led_strip_refresh(s_strip) : err;
}

static void led_task(void *arg)
{
    (void)arg;

    /* Power-on self test: red, green, blue, then off. */
    const color_t test[] = { COLOR_RED, COLOR_GREEN, COLOR_BLUE };
    for (size_t i = 0; i < sizeof(test) / sizeof(test[0]); i++) {
        show(test[i]);
        vTaskDelay(pdMS_TO_TICKS(SELFTEST_MS));
    }
    show(COLOR_OFF);

    bool connected = false;
    int64_t red_until = 0, green_until = 0; /* esp_timer microseconds */
    color_t shown = COLOR_OFF;
    bool retry = false; /* the last LED update failed */

    for (;;) {
        /* Sleep until the next flash ends, or until something happens. */
        const int64_t now = esp_timer_get_time();
        int64_t next = INT64_MAX;
        if (green_until > now && green_until < next) {
            next = green_until;
        }
        if (red_until > now && red_until < next) {
            next = red_until;
        }
        /* Round up to whole ticks so we never wake before the flash ends. */
        TickType_t wait = portMAX_DELAY;
        if (next != INT64_MAX) {
            const int64_t us = next - now;
            const int64_t tick_us = 1000000 / configTICK_RATE_HZ;
            wait = (TickType_t)((us + tick_us - 1) / tick_us);
        }
        if (retry) {
            wait = pdMS_TO_TICKS(50) < wait ? pdMS_TO_TICKS(50) : wait;
        }

        led_msg_t msg;
        if (xQueueReceive(s_queue, &msg, wait) == pdTRUE) {
            const int64_t t = esp_timer_get_time();
            switch (msg.type) {
            case EV_CONNECTED:    connected = true; break;
            case EV_DISCONNECTED: connected = false; red_until = green_until = 0; break;
            case EV_RX:           green_until = t + (int64_t)RX_FLASH_MS * 1000; break;
            case EV_TX: {
                const int64_t until = t + (int64_t)msg.ms * 1000;
                if (until > red_until) {
                    red_until = until;
                }
                break;
            }
            }
        }

        /* Green interrupts red briefly (e.g. a digipeated echo), red beats blue. */
        const int64_t t = esp_timer_get_time();
        color_t want = connected ? COLOR_BLUE : COLOR_OFF;
        if (t < red_until) {
            want = COLOR_RED;
        }
        if (t < green_until) {
            want = COLOR_GREEN;
        }
        if (want != shown || retry) {
            const esp_err_t err = show(want);
            ESP_LOGD(TAG, "%s -> %s (connected=%d, red %lld ms, green %lld ms)%s",
                     COLOR_NAMES[shown], COLOR_NAMES[want], connected,
                     red_until > t ? (long long)((red_until - t) / 1000) : 0LL,
                     green_until > t ? (long long)((green_until - t) / 1000) : 0LL,
                     err == ESP_OK ? "" : " FAILED");
            if (err == ESP_OK) {
                shown = want;
                retry = false;
            } else {
                ESP_LOGW(TAG, "LED update to %s failed: %s; retrying", COLOR_NAMES[want],
                         esp_err_to_name(err));
                retry = true;
            }
        }
    }
}

esp_err_t status_led_start(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = CONFIG_QRV_LINK_STATUS_LED_GPIO,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip), TAG,
                        "LED on GPIO %d", CONFIG_QRV_LINK_STATUS_LED_GPIO);

    s_queue = xQueueCreate(16, sizeof(led_msg_t));
    ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "queue");
    BaseType_t ok = xTaskCreate(led_task, "status_led", 3072, NULL, 3, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    ESP_LOGI(TAG, "status LED on GPIO %d, brightness %d", CONFIG_QRV_LINK_STATUS_LED_GPIO,
             CONFIG_QRV_LINK_STATUS_LED_BRIGHTNESS);
    return ESP_OK;
}

void status_led_set_connected(bool connected)
{
    post(connected ? EV_CONNECTED : EV_DISCONNECTED, 0);
}

void status_led_rx(void)
{
    post(EV_RX, 0);
}

void status_led_tx(uint32_t ms)
{
    post(EV_TX, ms);
}

#else /* LED disabled: keep the calls, do nothing. */

esp_err_t status_led_start(void) { return ESP_OK; }
void status_led_set_connected(bool connected) { (void)connected; }
void status_led_rx(void) {}
void status_led_tx(uint32_t ms) { (void)ms; }

#endif
