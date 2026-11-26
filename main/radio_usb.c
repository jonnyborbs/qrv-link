/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Jon Schulman
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "radio_usb.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/cdc_acm_host.h"
#include "usb/usb_host.h"

static const char *TAG = "radio_usb";

#define OPEN_TIMEOUT_MS   500  /* each cdc_acm_host_open() attempt waits this long */
#define USB_IN_BUF_SIZE   512
#define USB_OUT_BUF_SIZE  512
#define PORT_REINIT_MS    60000 /* re-initialize the root port this often while the radio is absent */
#define PORT_OFF_MS       200

#define EVT_DISCONNECTED  BIT0

static radio_usb_config_t s_config;
static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_lock;        /* guards s_dev against close during write */
static cdc_acm_dev_hdl_t s_dev;         /* NULL while the radio is not open */

/* Runs the USB host library's event loop for the lifetime of the firmware. */
static void usb_lib_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

/* CDC-ACM driver task context. */
static bool on_rx(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    ESP_LOG_BUFFER_HEXDUMP(TAG, data, len, ESP_LOG_DEBUG);
    if (s_config.rx_cb) {
        s_config.rx_cb(data, len, s_config.ctx);
    }
    return true; /* data consumed */
}

/* CDC-ACM driver task context: only signal, the radio_usb task does the close. */
static void on_event(const cdc_acm_host_dev_event_data_t *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "radio disconnected");
        xEventGroupSetBits(s_events, EVT_DISCONNECTED);
        break;
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC-ACM error %d", event->data.error);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGD(TAG, "serial state 0x%04x", event->data.serial_state.val);
        break;
    default:
        ESP_LOGD(TAG, "CDC-ACM event %d", event->type);
        break;
    }
}

static void radio_usb_task(void *arg)
{
    (void)arg;
    const cdc_acm_host_device_config_t dev_config = {
        .connection_timeout_ms = OPEN_TIMEOUT_MS,
        .out_buffer_size = USB_OUT_BUF_SIZE,
        .in_buffer_size = USB_IN_BUF_SIZE,
        .event_cb = on_event,
        .data_cb = on_rx,
        .user_arg = NULL,
    };
    bool waiting_logged = false;
    int64_t port_reinit_at = esp_timer_get_time();

    for (;;) {
        if (!waiting_logged) {
            ESP_LOGI(TAG, "waiting for radio (VID 0x%04x PID 0x%04x or 0x%04x) on the native USB port",
                     RADIO_USB_VID, RADIO_USB_PID_MAIN, RADIO_USB_PID_PANEL);
            waiting_logged = true;
        }

        /* Try the main unit's and the front panel's USB IDs in turn. */
        static const struct {
            uint16_t pid;
            const char *where;
        } ports[] = {
            { RADIO_USB_PID_MAIN, "main unit" },
            { RADIO_USB_PID_PANEL, "front panel" },
        };
        static size_t next_port;
        const size_t p = next_port;
        next_port = (next_port + 1) % (sizeof(ports) / sizeof(ports[0]));

        cdc_acm_dev_hdl_t dev = NULL;
        esp_err_t err = cdc_acm_host_open(RADIO_USB_VID, ports[p].pid, RADIO_USB_CDC_INTERFACE,
                                          &dev_config, &dev);
        if (err == ESP_ERR_NOT_FOUND) {
            /* Not there yet (open() already waited OPEN_TIMEOUT_MS). If it stays
             * away, re-initialize the root port now and then: once, after a radio
             * power-cycle, the radio only came back after an adapter reset. */
            if (esp_timer_get_time() - port_reinit_at >= (int64_t)PORT_REINIT_MS * 1000) {
                usb_host_lib_info_t info;
                if (usb_host_lib_info(&info) == ESP_OK && info.num_devices == 0) {
                    ESP_LOGI(TAG, "radio not seen for %d s; re-initializing the USB port",
                             PORT_REINIT_MS / 1000);
                    usb_host_lib_set_root_port_power(false);
                    vTaskDelay(pdMS_TO_TICKS(PORT_OFF_MS));
                    usb_host_lib_set_root_port_power(true);
                }
                port_reinit_at = esp_timer_get_time();
            }
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "open failed: %s; retrying", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        waiting_logged = false;
        ESP_LOGI(TAG, "radio CDC-ACM port opened (%s USB, PID 0x%04x)", ports[p].where,
                 ports[p].pid);

        if (esp_log_level_get(TAG) >= ESP_LOG_DEBUG) {
            cdc_acm_host_desc_print(dev);
        }

        /* Set line coding like a host serial driver does on open (ignored by
         * this virtual port). DTR starts deasserted: the radio only serves
         * CAT while DTR is high, and it needs a DTR rising edge after it is
         * ready, so the caller raises DTR via radio_usb_set_dtr(). */
        cdc_acm_line_coding_t coding = {
            .dwDTERate = 9600,
            .bCharFormat = 0, /* 1 stop bit */
            .bParityType = 0, /* none */
            .bDataBits = 8,
        };
        err = cdc_acm_host_line_coding_set(dev, &coding);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set line coding failed: %s (continuing)", esp_err_to_name(err));
        }
        err = cdc_acm_host_set_control_line_state(dev, false, false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "clear DTR/RTS failed: %s (continuing)", esp_err_to_name(err));
        }

        xEventGroupClearBits(s_events, EVT_DISCONNECTED);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_dev = dev;
        xSemaphoreGive(s_lock);

        if (s_config.state_cb) {
            s_config.state_cb(true, s_config.ctx);
        }

        xEventGroupWaitBits(s_events, EVT_DISCONNECTED, pdTRUE, pdFALSE, portMAX_DELAY);

        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_dev = NULL;
        xSemaphoreGive(s_lock);
        cdc_acm_host_close(dev);
        ESP_LOGI(TAG, "radio port closed");
        port_reinit_at = esp_timer_get_time(); /* give it PORT_REINIT_MS to come back */

        if (s_config.state_cb) {
            s_config.state_cb(false, s_config.ctx);
        }
    }
}

esp_err_t radio_usb_start(const radio_usb_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "config is NULL");
    ESP_RETURN_ON_FALSE(!s_events, ESP_ERR_INVALID_STATE, TAG, "already started");

    s_config = *config;
    s_events = xEventGroupCreate();
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_events && s_lock, ESP_ERR_NO_MEM, TAG, "no memory");

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_RETURN_ON_ERROR(usb_host_install(&host_config), TAG, "usb_host_install");

    BaseType_t ok = xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "usb_lib task");

    /* Default driver config: its own task, priority above ours. */
    ESP_RETURN_ON_ERROR(cdc_acm_host_install(NULL), TAG, "cdc_acm_host_install");

    ok = xTaskCreate(radio_usb_task, "radio_usb", 4096, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "radio_usb task");
    return ESP_OK;
}

bool radio_usb_is_connected(void)
{
    return s_dev != NULL;
}

esp_err_t radio_usb_set_dtr(bool dtr)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = s_dev ? cdc_acm_host_set_control_line_state(s_dev, dtr, false)
                          : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t radio_usb_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    if (!s_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = s_dev ? cdc_acm_host_data_tx_blocking(s_dev, data, len, timeout_ms)
                          : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_lock);
    return err;
}
