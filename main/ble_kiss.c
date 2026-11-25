/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Jon Schulman
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "ble_kiss.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_kiss";

/* 00000001-ba2a-46c9-ae49-01b0961f68bb and friends, little-endian byte order. */
#define KTS_UUID(n) BLE_UUID128_INIT(0xbb, 0x68, 0x1f, 0x96, 0xb0, 0x01, 0x49, 0xae, \
                                     0xc9, 0x46, 0x2a, 0xba, (n), 0x00, 0x00, 0x00)
static const ble_uuid128_t s_svc_uuid = KTS_UUID(0x01);
static const ble_uuid128_t s_tx_uuid = KTS_UUID(0x02);
static const ble_uuid128_t s_rx_uuid = KTS_UUID(0x03);

/* Two full worst-case KISS frames (2 * 660 bytes) plus slack. */
#define NOTIFY_BUF_SIZE   2048
#define NOTIFY_CHUNK_MAX  244 /* ATT MTU 247 - 3 */

#define CAPS_DESC_LABEL   "Write KISS data"

static ble_kiss_config_t s_config;
static char s_caps_desc[24]; /* e.g. "Write KISS data:03" */
static uint16_t s_rx_val_handle;
static uint8_t s_own_addr_type;
static StreamBufferHandle_t s_notify_buf;

/* Written from the host task, read from the notify task. */
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed;
static volatile uint16_t s_mtu = 23;

static void start_advertising(void);

/*
 * Restarting advertising from the DISCONNECT event can fail with
 * BLE_HS_ENOMEM: with one connection allowed, NimBLE may not have freed the old
 * connection yet. If advertising doesn't start, try again shortly, so the
 * adapter never stays invisible to apps. The retry uses a NimBLE callout so it
 * runs in the NimBLE host task (a FreeRTOS timer callback's small stack
 * overflows in ble_gap_adv_start).
 */
#define ADV_RETRY_MS 200
static struct ble_npl_callout s_adv_retry;
static bool s_adv_retry_ready;
static bool s_adv_failed; /* the last attempt failed; a retry is pending */

static void set_session(bool active)
{
    if (s_subscribed == active) {
        return;
    }
    s_subscribed = active;
    if (!active) {
        xStreamBufferReset(s_notify_buf);
    }
    ESP_LOGI(TAG, "app %s", active ? "subscribed: KISS session open" : "gone: KISS session closed");
    if (s_config.session_cb) {
        s_config.session_cb(active, s_config.ctx);
    }
}

static int chr_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint8_t buf[512];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (s_config.rx_cb && len > 0) {
            s_config.rx_cb(buf, len, s_config.ctx);
        }
        return 0;
    }
    case BLE_GATT_ACCESS_OP_READ_CHR:
        return 0; /* RX reads as empty; data only flows by notification */
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* TX characteristic User Description (0x2901): label + ":" + capability bitmask. */
static int caps_desc_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, s_caps_desc, strlen(s_caps_desc)) == 0
               ? 0
               : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    {
                        .uuid = BLE_UUID16_DECLARE(0x2901),
                        .att_flags = BLE_ATT_F_READ,
                        .access_cb = caps_desc_access,
                    },
                    { 0 },
                },
            },
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_rx_val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_mtu = 23;
            ESP_LOGI(TAG, "app connected (handle %d)", s_conn);
        } else {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "app disconnected (reason 0x%03x)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        set_session(false);
        start_advertising();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_rx_val_handle) {
            set_session(event->subscribe.cur_notify);
        }
        break;

    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "ATT MTU %d", s_mtu);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        start_advertising();
        break;

    default:
        break;
    }
    return 0;
}

static void adv_retry_cb(struct ble_npl_event *ev)
{
    (void)ev;
    start_advertising();
}

/* Schedule another attempt; logs only the first failure of a series. */
static void adv_failed(const char *what, int rc)
{
    if (!s_adv_failed) {
        ESP_LOGW(TAG, "%s failed (%d); retrying every %d ms", what, rc, ADV_RETRY_MS);
    }
    s_adv_failed = true;
    if (s_adv_retry_ready) {
        ble_npl_callout_reset(&s_adv_retry, ble_npl_time_ms_to_ticks32(ADV_RETRY_MS));
    }
}

static void start_advertising(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        s_adv_failed = false; /* an app is connected; advertising resumes when it leaves */
        return;
    }
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        adv_failed("setting advertising data", rc);
        return;
    }

    /* The name does not fit next to a 128-bit UUID; send it in the scan response. */
    struct ble_hs_adv_fields rsp = { 0 };
    const char *name = ble_svc_gap_device_name();
    rsp.name = (const uint8_t *)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        adv_failed("setting the scan response", rc);
        return;
    }

    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
    };
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        adv_failed("starting advertising", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\"%s", name, s_adv_failed ? " (after retrying)" : "");
    s_adv_failed = false;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "no usable BLE address: %d", rc);
        return;
    }
    uint8_t addr[6] = { 0 };
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE address %02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason %d", reason);
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run(); /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

static bool notify_chunk(const uint8_t *data, size_t len)
{
    for (int tries = 0; tries < 50; tries++) {
        uint16_t conn = s_conn;
        if (!s_subscribed || conn == BLE_HS_CONN_HANDLE_NONE) {
            return false;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        int rc = om ? ble_gatts_notify_custom(conn, s_rx_val_handle, om) : BLE_HS_ENOMEM;
        if (rc == 0) {
            return true;
        }
        if (rc != BLE_HS_ENOMEM) {
            ESP_LOGW(TAG, "notify failed: %d", rc);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10)); /* controller buffers full; retry */
    }
    ESP_LOGW(TAG, "notify gave up after retries; dropped %u bytes", (unsigned)len);
    return false;
}

/* Drains the notify buffer into ATT notifications sized to the current MTU. */
static void notify_task(void *arg)
{
    (void)arg;
    uint8_t chunk[NOTIFY_CHUNK_MAX];
    for (;;) {
        size_t n = xStreamBufferReceive(s_notify_buf, chunk, sizeof(chunk), portMAX_DELAY);
        /* Split by the MTU in effect now, not when the receive started. */
        for (size_t off = 0; off < n;) {
            size_t max = s_mtu > 3 ? s_mtu - 3 : 20;
            size_t len = n - off < max ? n - off : max;
            if (!notify_chunk(chunk + off, len)) {
                break; /* app went away or notify failed; drop the rest */
            }
            off += len;
        }
    }
}

esp_err_t ble_kiss_start(const ble_kiss_config_t *config)
{
    ESP_RETURN_ON_FALSE(config && config->device_name, ESP_ERR_INVALID_ARG, TAG, "bad config");
    s_config = *config;
    snprintf(s_caps_desc, sizeof(s_caps_desc), "%s:%02X", CAPS_DESC_LABEL, config->capabilities);
    ESP_LOGI(TAG, "capability descriptor: \"%s\"", s_caps_desc);

    s_notify_buf = xStreamBufferCreate(NOTIFY_BUF_SIZE, 1);
    ESP_RETURN_ON_FALSE(s_notify_buf, ESP_ERR_NO_MEM, TAG, "notify buffer");

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble_port_init");
    ble_npl_callout_init(&s_adv_retry, nimble_port_get_dflt_eventq(), adv_retry_cb, NULL);
    s_adv_retry_ready = true;

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_services);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "gatts count: %d", rc);
    rc = ble_gatts_add_svcs(s_services);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "gatts add: %d", rc);
    rc = ble_svc_gap_device_name_set(config->device_name);
    ESP_RETURN_ON_FALSE(rc == 0, ESP_FAIL, TAG, "device name: %d", rc);

    BaseType_t ok = xTaskCreate(notify_task, "ble_notify", 4096, NULL, 5, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "notify task");

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

size_t ble_kiss_send(const uint8_t *data, size_t len)
{
    if (!s_subscribed || !s_notify_buf) {
        return 0;
    }
    return xStreamBufferSend(s_notify_buf, data, len, 0);
}
