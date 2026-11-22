/*
 * SPDX-License-Identifier: MPL-2.0
 * Copyright (c) 2026 Jon Schulman
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include "bridge.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble_kiss.h"
#include "cat.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "radio_usb.h"
#include "status_led.h"
#include "sdkconfig.h"

static const char *TAG = "bridge";

#define CAT_TIMEOUT_MS     500
#define CAT_ATTEMPTS       3
#define WAKE_ATTEMPTS      20   /* DTR pulses before giving up */
#define SETTLE_MS          500  /* after USB open, before the first DTR pulse */
#define DTR_LOW_MS         250
#define DTR_HIGH_MS        250
#define KISS_EXIT_MS       300  /* wake path: after C0 FF C0, before talking CAT */
#define FAST_EXIT_MS       50   /* known-KISS exit: after C0 FF C0, before the ID probe */
#define FAST_EXIT_TRIES    3    /* ID probes after a known-KISS exit before pulsing DTR */
#define FAST_EXIT_ID_MS    300
#define KISS_GRACE_MS      3000 /* QSY build: stay in CAT this long after subscribe */
#define RETRY_PERIOD_MS    5000 /* re-try a radio that did not answer */
#define APP_TX_BUF_SIZE    4096 /* app -> radio; several queued frames */
#define APP_TX_WAIT_MS     5000 /* how long app data may wait for KISS to come up */

#define NVS_NAMESPACE      "qrvlink"
#define NVS_KEY_ORIG_TN    "orig_tn"
#define NVS_KEY_ORIG_QSY   "orig_qsy" /* "VM b,m;MR ccc;FQ b,f" saved before the first QSY */
#define NVS_KEY_ORIG_AS    "orig_as"  /* "AS n" saved before the first baud-rate change */

#define BIT_KISS_ACTIVE    BIT0
#define BIT_QSY_DONE       BIT1

#define KISS_FEND          0xC0
#define KISS_FESC          0xDB
#define KISS_TFEND         0xDC
#define KISS_TFESC         0xDD
#define KISS_CMD_SETHW     0x06
#define KISS_FRAME_MAX     1100 /* > 2 * 329 + 2 escaped worst case */
#define QSY_WAIT_MS        15000

/*
 * KISS SetHardware (0x06) sub-commands, as listed in
 * https://github.com/islandmagic/kiss-tnc-hw-cmd. The D750's own TNC ignores
 * them, so the adapter handles every 0x06 frame itself:
 *   C0 06 28 C0               Get firmware version -> C0 06 28 <string> C0
 *   C0 06 7B C0               Get API version      -> C0 06 7B <u16 MSB first> C0
 *   C0 06 7E C0               Get capabilities     -> C0 06 7E <u16 MSB first> C0
 *   C0 06 EA f3 f2 f1 f0 C0   Set frequency f (Hz, MSB first) on the TNC band, via CAT
 *   C0 06 EB C0               Restore the frequency from before the first EA
 *   C0 06 F4 nn C0            Set TNC baud rate: nn = 0 (1200) or 1 (9600), via CAT "AS"
 *                             (B.B. Link's extension; restored when the app leaves)
 * Others are logged and ignored.
 */
#define SETHW_GET_FW_VERSION   0x28
#define SETHW_GET_API_VERSION  0x7B
#define SETHW_GET_CAPABILITIES 0x7E
#define SETHW_QSY              0xEA
#define SETHW_QSY_RESTORE      0xEB
#define SETHW_SET_BAUD         0xF4

#define QRV_API_VERSION        0x0100 /* major.minor of the command set above */
#define CAP_FIRMWARE_VERSION   0x0800 /* bit meaning as defined by Mobilinkd */
#define QRV_CAPABILITIES       (CAP_FIRMWARE_VERSION)

#define REPLY_BOUNDARY_WAIT_MS 300 /* max wait for a radio frame to finish before replying */

static const uint8_t KISS_EXIT[] = { 0xC0, 0xFF, 0xC0 };

typedef enum {
    EV_USB_UP,
    EV_USB_DOWN,
    EV_SESSION_OPEN,
    EV_SESSION_CLOSE,
    EV_QSY,         /* arg: frequency in Hz */
    EV_QSY_RESTORE,
    EV_SET_BAUD,    /* arg: AS value, 0 = 1200, 1 = 9600 */
    EV_KISS_NEEDED, /* arg: 0 = grace period over, 1 = app sent data */
} bridge_event_t;

typedef struct {
    bridge_event_t type;
    uint32_t arg;
} bridge_msg_t;

static QueueHandle_t s_events;
static EventGroupHandle_t s_flags;
static StreamBufferHandle_t s_app_tx;

/* Routing flag read by the USB RX callback: true = radio bytes go to BLE. */
static volatile bool s_kiss_active;

/* Control task state. */
static bool s_usb_up;
static bool s_radio_ok;  /* answered CAT since the last USB_UP */
static bool s_want_kiss; /* an app is subscribed */
static bool s_in_kiss;   /* we put the radio's TNC into KISS */
static char s_orig_tn[16];
static uint32_t s_dropped_to_app;
static bool s_qsy_saved;     /* s_orig_vm / _mr / _fq hold the pre-QSY state (also in NVS) */
static char s_orig_vm[16];   /* e.g. "VM 1,1" (memory) or "VM 1,0" (VFO) */
static char s_orig_mr[16];   /* e.g. "MR 012", "MR AP" (special channel), or "N" on the VFO */
static char s_orig_fq[24];   /* e.g. "FQ 1,0144390000" (VFO frequency) */
static bool s_baud_saved;    /* s_orig_as holds the pre-session TNC speed (also in NVS) */
static char s_orig_as[8];    /* e.g. "AS 0" */
static bool s_kiss_needed;   /* the session needs the TNC in KISS now (see lazy entry) */
static TimerHandle_t s_grace_timer;

/*
 * KISS parameter frames (TXDELAY, P, SlotTime, TXtail, FullDuplex) that arrive
 * while the radio is still in CAT mode are kept here and replayed when KISS
 * comes up, newest value per parameter. Guarded by s_param_lock together with
 * the switch to KISS, so a frame is either replayed or forwarded, never lost.
 */
#define KISS_PARAM_SLOTS   6
#define KISS_PARAM_MAXLEN  8
typedef struct {
    uint8_t len;
    uint8_t data[KISS_PARAM_MAXLEN];
} kiss_param_t;
static kiss_param_t s_params[KISS_PARAM_SLOTS];
static SemaphoreHandle_t s_param_lock;

/* ---------- callbacks (other tasks' context) ---------- */

static bool post_arg(bridge_event_t type, uint32_t arg)
{
    const bridge_msg_t msg = { .type = type, .arg = arg };
    if (xQueueSend(s_events, &msg, 0) != pdTRUE) {
        ESP_LOGE(TAG, "event queue full, lost event %d", type);
        return false;
    }
    return true;
}

static void post(bridge_event_t type)
{
    post_arg(type, 0);
}

/*
 * Everything sent to the app goes through s_to_app_lock, so adapter replies
 * and radio bytes never interleave. s_radio_mid_frame is true while the radio
 * stream to the app has an open KISS frame (the last chunk did not end on a
 * FEND); replies wait for it to close.
 */
static SemaphoreHandle_t s_to_app_lock;
static bool s_radio_mid_frame;

/* For the status LED: radio -> app frame tracking (USB task only), and what the
 * TX-time estimate needs. */
static size_t s_rx_frame_len;      /* bytes since the last FEND */
static bool s_rx_frame_is_data;    /* first byte of the current frame was a data command */
static volatile uint32_t s_txdelay_ms = 300; /* from the app's KISS TXDELAY, if it sends one */
static volatile uint32_t s_tnc_baud = 1200;  /* from CAT AS, read whenever KISS is switched on */

/* Flash green for each complete data frame the radio delivers. */
static void led_track_rx(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (data[i] == KISS_FEND) {
            if (s_rx_frame_len > 0 && s_rx_frame_is_data) {
                status_led_rx();
            }
            s_rx_frame_len = 0;
        } else {
            if (s_rx_frame_len == 0) {
                s_rx_frame_is_data = (data[i] & 0x0F) == 0x00;
            }
            s_rx_frame_len++;
        }
    }
}

/* Rough time the radio needs to send a KISS data frame of `frame_len` bytes
 * (FEND to FEND): TXDELAY plus the AX.25 frame at the TNC's speed, allowing
 * for flags, FCS and bit stuffing. KISS gives no "transmission done" signal. */
static uint32_t estimate_tx_ms(size_t frame_len)
{
    const uint32_t payload = frame_len > 3 ? frame_len - 3 : 0; /* minus FENDs and command */
    const uint32_t bits = (payload + 4) * 8 * 11 / 10;             /* + flags/FCS, ~10% stuffing */
    return s_txdelay_ms + bits * 1000 / s_tnc_baud + 50;
}

static void on_radio_rx(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (s_kiss_active) {
        xSemaphoreTake(s_to_app_lock, portMAX_DELAY);
        size_t sent = ble_kiss_send(data, len);
        if (len > 0) {
            s_radio_mid_frame = data[len - 1] != 0xC0;
        }
        xSemaphoreGive(s_to_app_lock);
        led_track_rx(data, len);
        if (sent < len) {
            s_dropped_to_app += len - sent;
        }
    } else {
        cat_feed(data, len);
    }
}

static void on_radio_state(bool connected, void *ctx)
{
    (void)ctx;
    post(connected ? EV_USB_UP : EV_USB_DOWN);
}

static void on_app_rx(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
#if CONFIG_QRV_LINK_LOG_APP_WRITES
    ESP_LOGI(TAG, "app wrote %u bytes%s", (unsigned)len, s_kiss_active ? "" : " (KISS not active)");
    ESP_LOG_BUFFER_HEXDUMP(TAG, data, len < 64 ? len : 64, ESP_LOG_INFO);
#endif
    size_t n = xStreamBufferSend(s_app_tx, data, len, 0);
    if (n < len) {
        ESP_LOGW(TAG, "app TX buffer full, dropped %u bytes", (unsigned)(len - n));
    }
}

static void on_app_session(bool active, void *ctx)
{
    (void)ctx;
    status_led_set_connected(active);
    post(active ? EV_SESSION_OPEN : EV_SESSION_CLOSE);
}

/* ---------- NVS: radio state to restore after an interrupted session ---------- */

static bool nvs_load_key(const char *key, char *out, size_t size)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = size;
    esp_err_t err = nvs_get_str(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK;
}

/* value NULL erases the key. */
static void nvs_store_key(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (value) {
        nvs_set_str(h, key, value);
    } else {
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
}

static bool nvs_load_orig(char *out, size_t size)
{
    return nvs_load_key(NVS_KEY_ORIG_TN, out, size);
}

static void nvs_store_orig(const char *value)
{
    nvs_store_key(NVS_KEY_ORIG_TN, value);
}

static void nvs_store_qsy(bool saved)
{
    if (!saved) {
        nvs_store_key(NVS_KEY_ORIG_QSY, NULL);
        return;
    }
    char value[64];
    snprintf(value, sizeof(value), "%s;%s;%s", s_orig_vm, s_orig_mr, s_orig_fq);
    nvs_store_key(NVS_KEY_ORIG_QSY, value);
}

/* Load the pre-QSY state left by an interrupted session, if any. */
static bool nvs_load_qsy(void)
{
    char value[64];
    if (!nvs_load_key(NVS_KEY_ORIG_QSY, value, sizeof(value))) {
        return false;
    }
    char *mr = strchr(value, ';');
    char *fq = mr ? strchr(mr + 1, ';') : NULL;
    if (!mr || !fq || strncmp(value, "VM ", 3) != 0 || strncmp(fq + 1, "FQ ", 3) != 0) {
        nvs_store_key(NVS_KEY_ORIG_QSY, NULL); /* unreadable; drop it */
        return false;
    }
    *mr++ = '\0';
    *fq++ = '\0';
    strlcpy(s_orig_vm, value, sizeof(s_orig_vm));
    strlcpy(s_orig_mr, mr, sizeof(s_orig_mr));
    strlcpy(s_orig_fq, fq, sizeof(s_orig_fq));
    return true;
}

/* ---------- CAT helpers ---------- */

/* A reply belongs to `cmd` if it echoes the 2-letter command or is "?".
 * MR answers a bare "N" when the band is on the VFO. */
static bool reply_matches(const char *cmd, const char *reply)
{
    return strcmp(reply, "?") == 0 || strncmp(reply, cmd, 2) == 0 ||
           (strncmp(cmd, "MR", 2) == 0 && strcmp(reply, "N") == 0);
}

static esp_err_t cat_query(const char *cmd, char *reply, size_t reply_size)
{
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= CAT_ATTEMPTS; attempt++) {
        err = cat_command(cmd, reply, reply_size, CAT_TIMEOUT_MS);
        if (err == ESP_OK && reply_matches(cmd, reply)) {
            return ESP_OK;
        }
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "%s: ignoring stray reply '%s'", cmd, reply);
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    return err;
}

/* "TN p1,p2" -> true if well formed. */
static bool parse_tn(const char *reply, char *mode, char *band)
{
    if (strlen(reply) != 6 || strncmp(reply, "TN ", 3) != 0 || reply[4] != ',') {
        return false;
    }
    *mode = reply[3];
    *band = reply[5];
    return true;
}

static void set_kiss_active(bool active)
{
    s_radio_mid_frame = false;
    s_rx_frame_len = 0;
    s_kiss_active = active;
    if (active) {
        xEventGroupSetBits(s_flags, BIT_KISS_ACTIVE);
    } else {
        xEventGroupClearBits(s_flags, BIT_KISS_ACTIVE);
    }
}

/* Send the KISS exit frame. A trailing CR flushes it from the CAT parser in
 * case the radio was not in KISS after all. */
static void send_kiss_exit(void)
{
    radio_usb_write(KISS_EXIT, sizeof(KISS_EXIT), 500);
    vTaskDelay(pdMS_TO_TICKS(KISS_EXIT_MS));
    radio_usb_write((const uint8_t *)"\r", 1, 500);
    vTaskDelay(pdMS_TO_TICKS(100));
    cat_reset();
}

/*
 * Bring the radio's CAT port up: the TM-D750 only answers after a DTR rising
 * edge once it is ready. If it still does not answer it is probably stuck in
 * KISS from an earlier session (KISS survives DTR drops and replugs), so send
 * the KISS exit frame and keep trying.
 */
static bool wake_radio(void)
{
    char reply[CAT_LINE_MAX];
    for (int attempt = 1; attempt <= WAKE_ATTEMPTS && radio_usb_is_connected(); attempt++) {
        radio_usb_set_dtr(false);
        vTaskDelay(pdMS_TO_TICKS(DTR_LOW_MS));
        radio_usb_set_dtr(true);
        vTaskDelay(pdMS_TO_TICKS(DTR_HIGH_MS));
        cat_reset();
        if (cat_command("ID", reply, sizeof(reply), CAT_TIMEOUT_MS) == ESP_OK &&
            strncmp(reply, "ID ", 3) == 0) {
            ESP_LOGI(TAG, "radio answering (%s) after %d DTR pulse(s)", reply, attempt);
            return true;
        }
        ESP_LOGI(TAG, "no CAT reply; sending KISS exit in case the TNC is in KISS");
        send_kiss_exit();
    }
    return false;
}

/*
 * Leave KISS when we know the TNC is in it, and wait for CAT. After the exit
 * frame the radio answers CAT straight away (DTR is still high), so probe with
 * ID first and only fall back to the slow DTR wake if that fails.
 */
static bool exit_kiss_to_cat(void)
{
    const int64_t t0 = esp_timer_get_time();
    set_kiss_active(false);
    s_in_kiss = false;
    radio_usb_write(KISS_EXIT, sizeof(KISS_EXIT), 500);
    vTaskDelay(pdMS_TO_TICKS(FAST_EXIT_MS));
    char reply[CAT_LINE_MAX];
    for (int attempt = 1; attempt <= FAST_EXIT_TRIES; attempt++) {
        cat_reset();
        if (cat_command("ID", reply, sizeof(reply), FAST_EXIT_ID_MS) == ESP_OK &&
            strncmp(reply, "ID ", 3) == 0) {
            ESP_LOGI(TAG, "KISS exit -> CAT in %lld ms (probe %d)",
                     (long long)((esp_timer_get_time() - t0) / 1000), attempt);
            return true;
        }
    }
    ESP_LOGW(TAG, "no CAT reply after KISS exit; falling back to DTR wake");
    send_kiss_exit();
    return wake_radio();
}

/* ---------- state transitions (control task only) ---------- */

static void radio_bringup(void)
{
    s_radio_ok = false;
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));
    if (!wake_radio()) {
        ESP_LOGE(TAG, "radio does not answer CAT on USB; will retry");
        return;
    }

    char reply[CAT_LINE_MAX];
    if (cat_query("FQ 0", reply, sizeof(reply)) == ESP_OK) {
        ESP_LOGI(TAG, "band A: %s", reply);
    }

    char mode, band;
    if (cat_query("TN", reply, sizeof(reply)) != ESP_OK || !parse_tn(reply, &mode, &band)) {
        ESP_LOGE(TAG, "cannot read TNC mode (TN)");
        return;
    }
    char saved[sizeof(s_orig_tn)];
    if (nvs_load_orig(saved, sizeof(saved))) {
        /* A session was interrupted: the current mode is ours, not the user's. */
        strlcpy(s_orig_tn, saved, sizeof(s_orig_tn));
        ESP_LOGW(TAG, "recovering from an interrupted session; radio says %s, original %s",
                 reply, s_orig_tn);
    } else {
        strlcpy(s_orig_tn, reply, sizeof(s_orig_tn));
        ESP_LOGI(TAG, "radio TNC mode: %s", s_orig_tn);
    }
    if (nvs_load_key(NVS_KEY_ORIG_AS, s_orig_as, sizeof(s_orig_as))) {
        s_baud_saved = true;
        ESP_LOGW(TAG, "recovering the TNC speed from an interrupted session: %s", s_orig_as);
    }
    if (nvs_load_qsy()) {
        s_qsy_saved = true;
        ESP_LOGW(TAG, "recovering the band from an interrupted session: %s, %s, %s",
                 s_orig_vm, s_orig_mr, s_orig_fq);
    }
    s_in_kiss = false;
    s_radio_ok = true;
}

/*
 * Decide which TNC mode to restore when the session ends. The flash copy
 * wins when it exists: it is set while a session is in progress (e.g. when
 * re-entering KISS after a QSY) or left by an interrupted one. Otherwise read
 * the radio now, so a mode the user changed on the radio since the adapter
 * booted (say, APRS turned on) is what gets restored.
 */
static bool capture_orig_tn(void)
{
    char saved[sizeof(s_orig_tn)];
    if (nvs_load_orig(saved, sizeof(saved))) {
        strlcpy(s_orig_tn, saved, sizeof(s_orig_tn));
        return true;
    }
    char reply[CAT_LINE_MAX], mode, band;
    if (cat_query("TN", reply, sizeof(reply)) != ESP_OK || !parse_tn(reply, &mode, &band)) {
        ESP_LOGE(TAG, "cannot read TNC mode (TN) at session start");
        return false;
    }
    strlcpy(s_orig_tn, reply, sizeof(s_orig_tn));
    ESP_LOGI(TAG, "session start: radio TNC mode %s", s_orig_tn);
    return true;
}

static bool enter_kiss(void)
{
    if (!capture_orig_tn()) {
        return false;
    }
    char as[CAT_LINE_MAX];
    if (cat_query("AS", as, sizeof(as)) == ESP_OK && strncmp(as, "AS ", 3) == 0) {
        s_tnc_baud = as[3] == '1' ? 9600 : 1200; /* for the status LED's TX estimate */
    }
    char mode, band;
    if (!parse_tn(s_orig_tn, &mode, &band)) {
        band = '1';
    }
    char cmd[16], reply[CAT_LINE_MAX];
    snprintf(cmd, sizeof(cmd), "TN 2,%c", band);

    nvs_store_orig(s_orig_tn); /* before switching, so a reset can undo it */
    if (cat_query(cmd, reply, sizeof(reply)) != ESP_OK || strcmp(reply, cmd) != 0) {
        ESP_LOGE(TAG, "radio refused %s", cmd);
        nvs_store_orig(NULL);
        return false;
    }
    s_in_kiss = true;
    s_dropped_to_app = 0;

    int replayed = 0;
    xSemaphoreTake(s_param_lock, portMAX_DELAY);
    for (int i = 0; i < KISS_PARAM_SLOTS; i++) {
        if (s_params[i].len) {
            radio_usb_write(s_params[i].data, s_params[i].len, 500);
            replayed++;
        }
    }
    set_kiss_active(true);
    xSemaphoreGive(s_param_lock);

    ESP_LOGI(TAG, "KISS on (band %c); bridging app <-> radio%s", band == '0' ? 'A' : 'B',
             replayed ? " (replayed app KISS parameters)" : "");
    return true;
}

/* Put the TNC back the way the user had it and forget the saved mode. */
static void restore_tnc(void)
{
    char reply[CAT_LINE_MAX];
    if (cat_query("TN", reply, sizeof(reply)) == ESP_OK && strcmp(reply, s_orig_tn) != 0) {
        if (cat_query(s_orig_tn, reply, sizeof(reply)) != ESP_OK ||
            strcmp(reply, s_orig_tn) != 0) {
            ESP_LOGE(TAG, "could not restore %s", s_orig_tn);
            return;
        }
    }
    nvs_store_orig(NULL);
    ESP_LOGI(TAG, "radio TNC mode restored: %s", s_orig_tn);
}

static char tnc_band(void)
{
    char mode, band;
    return parse_tn(s_orig_tn, &mode, &band) ? band : '1';
}

static bool is_channel_number(const char *ch)
{
    return strlen(ch) == 3 && ch[0] >= '0' && ch[0] <= '9' && ch[1] >= '0' && ch[1] <= '9' &&
           ch[2] >= '0' && ch[2] <= '9';
}

/*
 * CAT mode only. Put the band back exactly as it was before the first QSY:
 * VFO frequency, VFO/memory mode, and the memory channel. Numbered channels
 * are re-selected explicitly (MR b,nnn); special ones such as the D750's APRS
 * channel ("MR AP") can't be set over CAT, but switching back to memory mode
 * returns the band to them. Logs the band's final state.
 */
static void qsy_restore_cat(void)
{
    if (!s_qsy_saved) {
        return;
    }
    const char band = s_orig_vm[3];
    char cmd[24], reply[CAT_LINE_MAX];

    /* The VFO frequency can only be set on the VFO. */
    snprintf(cmd, sizeof(cmd), "VM %c,0", band);
    cat_query(cmd, reply, sizeof(reply));
    if (cat_query(s_orig_fq, reply, sizeof(reply)) != ESP_OK || strcmp(reply, s_orig_fq) != 0) {
        ESP_LOGE(TAG, "QSY restore: %s failed (%s)", s_orig_fq, reply);
    }

    const bool was_memory = s_orig_vm[strlen(s_orig_vm) - 1] != '0';
    if (was_memory) {
        if (cat_query(s_orig_vm, reply, sizeof(reply)) != ESP_OK || strcmp(reply, s_orig_vm) != 0) {
            ESP_LOGE(TAG, "QSY restore: %s failed (%s)", s_orig_vm, reply);
        }
        snprintf(cmd, sizeof(cmd), "MR %c", band);
        if (strncmp(s_orig_mr, "MR ", 3) == 0 &&
            cat_query(cmd, reply, sizeof(reply)) == ESP_OK && strcmp(reply, s_orig_mr) != 0) {
            const char *ch = s_orig_mr + 3;
            if (is_channel_number(ch)) {
                snprintf(cmd, sizeof(cmd), "MR %c,%s", band, ch);
                if (cat_query(cmd, reply, sizeof(reply)) != ESP_OK || strcmp(reply, cmd) != 0) {
                    ESP_LOGE(TAG, "QSY restore: %s failed (%s)", cmd, reply);
                }
            } else {
                ESP_LOGW(TAG, "QSY restore: band is on %s, was %s (can't select that channel over CAT)",
                         reply, s_orig_mr);
            }
        }
    }

    /* Read back and report where the band actually is now. */
    char vm[16] = "", mr[16] = "", fq[24] = "";
    snprintf(cmd, sizeof(cmd), "VM %c", band);
    cat_query(cmd, vm, sizeof(vm));
    snprintf(cmd, sizeof(cmd), "MR %c", band);
    cat_query(cmd, mr, sizeof(mr));
    snprintf(cmd, sizeof(cmd), "FQ %c", band);
    cat_query(cmd, fq, sizeof(fq));
    const bool ok = strcmp(vm, s_orig_vm) == 0 && strcmp(mr, s_orig_mr) == 0;
    if (ok) {
        ESP_LOGI(TAG, "QSY restored: band %c is back on %s, %s, %s", band == '0' ? 'A' : 'B', vm,
                 mr, fq);
    } else {
        ESP_LOGW(TAG, "QSY restore incomplete: band now %s, %s, %s; was %s, %s, %s", vm, mr, fq,
                 s_orig_vm, s_orig_mr, s_orig_fq);
    }
    s_qsy_saved = false;
    nvs_store_qsy(false);
}

/* CAT mode only. Switch the TNC band to VFO and tune it to `hz`. */
static bool qsy_set_cat(uint32_t hz)
{
    const char band = tnc_band();
    char cmd[32], reply[CAT_LINE_MAX];

    if (!s_qsy_saved) {
        snprintf(cmd, sizeof(cmd), "VM %c", band);
        if (cat_query(cmd, s_orig_vm, sizeof(s_orig_vm)) != ESP_OK ||
            strncmp(s_orig_vm, "VM ", 3) != 0) {
            ESP_LOGE(TAG, "QSY: cannot read %s (%s)", cmd, s_orig_vm);
            return false;
        }
        snprintf(cmd, sizeof(cmd), "MR %c", band);
        if (cat_query(cmd, s_orig_mr, sizeof(s_orig_mr)) != ESP_OK) {
            strlcpy(s_orig_mr, "N", sizeof(s_orig_mr));
        }
    }
    /* FQ only works on a VFO. */
    snprintf(cmd, sizeof(cmd), "VM %c,0", band);
    if (cat_query(cmd, reply, sizeof(reply)) != ESP_OK || strcmp(reply, cmd) != 0) {
        ESP_LOGE(TAG, "QSY: %s refused (%s)", cmd, reply);
        return false;
    }
    if (!s_qsy_saved) {
        snprintf(cmd, sizeof(cmd), "FQ %c", band);
        if (cat_query(cmd, s_orig_fq, sizeof(s_orig_fq)) != ESP_OK ||
            strncmp(s_orig_fq, "FQ ", 3) != 0) {
            ESP_LOGE(TAG, "QSY: cannot read %s (%s)", cmd, s_orig_fq);
            return false;
        }
        s_qsy_saved = true;
        nvs_store_qsy(true); /* so a reset mid-session can still restore the channel */
        ESP_LOGI(TAG, "QSY: saved %s, %s, %s", s_orig_vm, s_orig_mr, s_orig_fq);
    }
    snprintf(cmd, sizeof(cmd), "FQ %c,%010lu", band, (unsigned long)hz);
    if (cat_query(cmd, reply, sizeof(reply)) != ESP_OK || strcmp(reply, cmd) != 0) {
        ESP_LOGE(TAG, "QSY: %s refused (%s)", cmd, reply);
        return false;
    }
    ESP_LOGI(TAG, "QSY: band %c now %lu.%06lu MHz", band == '0' ? 'A' : 'B',
             (unsigned long)(hz / 1000000), (unsigned long)(hz % 1000000));
    return true;
}

/* CAT mode only. Set the TNC speed (AS 0 = 1200, AS 1 = 9600), saving the original first. */
static void baud_set_cat(uint32_t as_value)
{
    char cmd[8], reply[CAT_LINE_MAX];
    if (!s_baud_saved) {
        if (cat_query("AS", s_orig_as, sizeof(s_orig_as)) != ESP_OK ||
            strncmp(s_orig_as, "AS ", 3) != 0) {
            ESP_LOGE(TAG, "baud: cannot read AS (%s)", s_orig_as);
            return;
        }
        s_baud_saved = true;
        nvs_store_key(NVS_KEY_ORIG_AS, s_orig_as);
    }
    snprintf(cmd, sizeof(cmd), "AS %lu", (unsigned long)as_value);
    if (cat_query(cmd, reply, sizeof(reply)) != ESP_OK || strcmp(reply, cmd) != 0) {
        ESP_LOGE(TAG, "baud: %s refused (%s)", cmd, reply);
        return;
    }
    ESP_LOGI(TAG, "TNC speed now %s baud (was %s)", as_value ? "9600" : "1200", s_orig_as);
}

/* CAT mode only. Put the TNC speed back if a session changed it. */
static void baud_restore_cat(void)
{
    if (!s_baud_saved) {
        return;
    }
    char reply[CAT_LINE_MAX];
    if (cat_query("AS", reply, sizeof(reply)) == ESP_OK && strcmp(reply, s_orig_as) != 0 &&
        (cat_query(s_orig_as, reply, sizeof(reply)) != ESP_OK || strcmp(reply, s_orig_as) != 0)) {
        ESP_LOGE(TAG, "baud: could not restore %s (%s)", s_orig_as, reply);
        return;
    }
    ESP_LOGI(TAG, "TNC speed restored: %s", s_orig_as);
    s_baud_saved = false;
    nvs_store_key(NVS_KEY_ORIG_AS, NULL);
}

/*
 * Run a QSY step over CAT. If KISS is on, leave it for the CAT step and go
 * back afterwards. With CONFIG_QRV_LINK_LAZY_KISS the radio is usually still in
 * CAT when the QSY arrives, so no mode change is needed, and after a restore it
 * stays in CAT (the app is about to leave) until the app sends data again.
 * Control task only.
 */
static void qsy_handle(bridge_event_t type, uint32_t hz)
{
    if (!s_usb_up || !s_radio_ok || !s_want_kiss) {
        ESP_LOGW(TAG, "radio command ignored: no app session");
        return;
    }
    const bool was_kiss = s_in_kiss;
    if (was_kiss && !exit_kiss_to_cat()) {
        ESP_LOGE(TAG, "radio silent after KISS exit");
        s_radio_ok = false;
        return;
    }
    if (type == EV_QSY || type == EV_SET_BAUD) {
        if (type == EV_QSY) {
            qsy_set_cat(hz);
        } else {
            baud_set_cat(hz);
        }
        if (was_kiss) {
            enter_kiss();
        }
    } else {
        qsy_restore_cat();
#if CONFIG_QRV_LINK_LAZY_KISS
        /* The app is usually about to leave: stay in CAT until it sends data. */
        s_kiss_needed = false;
        xTimerReset(s_grace_timer, 0);
#else
        if (was_kiss) {
            enter_kiss();
        }
#endif
    }
}

static void leave_kiss(void)
{
    if (s_dropped_to_app) {
        ESP_LOGW(TAG, "%lu radio bytes dropped (app too slow)", (unsigned long)s_dropped_to_app);
    }
    if (!exit_kiss_to_cat()) {
        ESP_LOGE(TAG, "radio silent after KISS exit");
        s_radio_ok = false;
        return;
    }
    qsy_restore_cat();
    baud_restore_cat();
    restore_tnc();
    ESP_LOGI(TAG, "KISS off");
}

static void reconcile(void)
{
    if (!s_usb_up || !s_radio_ok) {
        return;
    }
    if (s_want_kiss && s_kiss_needed && !s_in_kiss) {
        enter_kiss();
    } else if (!s_want_kiss && s_in_kiss) {
        leave_kiss();
    } else if (!s_want_kiss && !s_in_kiss) {
        /* Session over without KISS on (lazy entry), or an interrupted one. */
        qsy_restore_cat();
        baud_restore_cat();
        char saved[sizeof(s_orig_tn)];
        if (nvs_load_orig(saved, sizeof(saved))) {
            restore_tnc();
        }
    }
}

static void control_task(void *arg)
{
    (void)arg;
    for (;;) {
        bridge_msg_t msg;
        if (xQueueReceive(s_events, &msg, pdMS_TO_TICKS(RETRY_PERIOD_MS)) == pdTRUE) {
            switch (msg.type) {
            case EV_USB_UP:
                ESP_LOGI(TAG, "radio USB up");
                s_usb_up = true;
                radio_bringup();
                break;
            case EV_USB_DOWN:
                ESP_LOGI(TAG, "radio USB down");
                s_usb_up = false;
                s_radio_ok = false;
                s_in_kiss = false; /* radio may still be in KISS; NVS remembers */
                set_kiss_active(false);
                break;
            case EV_SESSION_OPEN:
                s_want_kiss = true;
#if CONFIG_QRV_LINK_LAZY_KISS
                /* Lazy entry: a QSY usually follows within ~2 s and needs CAT,
                 * so don't switch to KISS until the app sends data or the
                 * grace period ends. */
                s_kiss_needed = false;
                xTimerReset(s_grace_timer, 0);
                ESP_LOGI(TAG, "app session: staying in CAT until data or %d ms", KISS_GRACE_MS);
#else
                s_kiss_needed = true;
#endif
                break;
            case EV_SESSION_CLOSE:
                s_want_kiss = false;
                s_kiss_needed = false;
                xTimerStop(s_grace_timer, 0);
                xSemaphoreTake(s_param_lock, portMAX_DELAY);
                memset(s_params, 0, sizeof(s_params));
                xSemaphoreGive(s_param_lock);
                break;
            case EV_KISS_NEEDED:
                if (s_want_kiss && !s_kiss_needed) {
                    s_kiss_needed = true;
                    ESP_LOGI(TAG, "entering KISS: %s",
                             msg.arg ? "app sent data" : "grace period over");
                }
                break;
            case EV_QSY:
            case EV_QSY_RESTORE:
            case EV_SET_BAUD:
                qsy_handle(msg.type, msg.arg);
                xEventGroupSetBits(s_flags, BIT_QSY_DONE);
                break;
            }
        } else if (s_usb_up && !s_radio_ok) {
            radio_bringup();
        }
        reconcile();
    }
}

/* Writes app bytes to the radio once KISS is up. app_tx task only. */
static bool forward_to_radio(const uint8_t *data, size_t len)
{
    EventBits_t bits = xEventGroupWaitBits(s_flags, BIT_KISS_ACTIVE, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(APP_TX_WAIT_MS));
    if (!(bits & BIT_KISS_ACTIVE)) {
        ESP_LOGW(TAG, "KISS not active; dropped %u bytes from app", (unsigned)len);
        return false;
    }
    esp_err_t err = radio_usb_write(data, len, 2000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write to radio failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/*
 * Send an adapter-generated KISS frame (a SetHardware reply) to the app. Waits
 * briefly for any radio frame in flight to finish so the two never interleave.
 * app_tx task only.
 */
static void reply_to_app(uint8_t subcmd, const uint8_t *data, size_t len)
{
    uint8_t frame[80];
    size_t n = 0;
    frame[n++] = KISS_FEND;
    frame[n++] = KISS_CMD_SETHW;
    const uint8_t first = subcmd;
    for (size_t i = 0; i <= len && n < sizeof(frame) - 3; i++) {
        const uint8_t c = i == 0 ? first : data[i - 1];
        if (c == KISS_FEND) {
            frame[n++] = KISS_FESC;
            frame[n++] = KISS_TFEND;
        } else if (c == KISS_FESC) {
            frame[n++] = KISS_FESC;
            frame[n++] = KISS_TFESC;
        } else {
            frame[n++] = c;
        }
    }
    frame[n++] = KISS_FEND;

    for (int waited = 0;; waited += 10) {
        xSemaphoreTake(s_to_app_lock, portMAX_DELAY);
        if (!s_kiss_active || !s_radio_mid_frame || waited >= REPLY_BOUNDARY_WAIT_MS) {
            size_t sent = ble_kiss_send(frame, n);
            xSemaphoreGive(s_to_app_lock);
            if (sent < n) {
                ESP_LOGW(TAG, "reply 0x%02x not delivered (no subscriber or buffer full)", subcmd);
            }
            return;
        }
        xSemaphoreGive(s_to_app_lock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void reply16_to_app(uint8_t subcmd, uint16_t value)
{
    const uint8_t data[2] = { (uint8_t)(value >> 8), (uint8_t)(value & 0xFF) };
    reply_to_app(subcmd, data, sizeof(data));
}

/* Hand a CAT step to the control task and wait for it, so later frames are
 * not written while the radio is out of KISS. */
static void run_qsy_step(bridge_event_t type, uint32_t hz)
{
    xEventGroupClearBits(s_flags, BIT_QSY_DONE);
    if (post_arg(type, hz)) {
        xEventGroupWaitBits(s_flags, BIT_QSY_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(QSY_WAIT_MS));
    }
}

/* A SetHardware frame from the app: handle the ones we know, never pass to the radio. */
static void handle_sethw(const uint8_t *p, size_t n)
{
    if (n >= 1 && p[0] == SETHW_GET_FW_VERSION) {
        const char *version = esp_app_get_description()->version;
        ESP_LOGI(TAG, "app asked for firmware version: %s", version);
        reply_to_app(SETHW_GET_FW_VERSION, (const uint8_t *)version, strlen(version));
    } else if (n >= 1 && p[0] == SETHW_GET_API_VERSION) {
        ESP_LOGI(TAG, "app asked for API version: 0x%04x", QRV_API_VERSION);
        reply16_to_app(SETHW_GET_API_VERSION, QRV_API_VERSION);
    } else if (n >= 1 && p[0] == SETHW_GET_CAPABILITIES) {
        ESP_LOGI(TAG, "app asked for capabilities: 0x%04x", QRV_CAPABILITIES);
        reply16_to_app(SETHW_GET_CAPABILITIES, QRV_CAPABILITIES);
    } else if (n >= 5 && p[0] == SETHW_QSY) {
        uint32_t hz = ((uint32_t)p[1] << 24) | ((uint32_t)p[2] << 16) |
                      ((uint32_t)p[3] << 8) | p[4];
        ESP_LOGI(TAG, "app QSY request: %lu Hz", (unsigned long)hz);
        run_qsy_step(EV_QSY, hz);
    } else if (n >= 2 && p[0] == SETHW_SET_BAUD && p[1] <= 1) {
        ESP_LOGI(TAG, "app baud rate request: %s", p[1] ? "9600" : "1200");
        run_qsy_step(EV_SET_BAUD, p[1]);
    } else if (n >= 1 && p[0] == SETHW_QSY_RESTORE) {
        ESP_LOGI(TAG, "app QSY restore request");
        run_qsy_step(EV_QSY_RESTORE, 0);
    } else {
        ESP_LOGI(TAG, "app SetHardware 0x%02x (%u bytes) ignored", n ? p[0] : 0, (unsigned)n);
    }
}

/* One complete frame, FEND to FEND. */
static void dispatch_frame(const uint8_t *f, size_t n)
{
    if (n >= 3 && (f[1] & 0x0F) == KISS_CMD_SETHW) {
        uint8_t payload[16];
        size_t len = 0;
        bool esc = false;
        for (size_t i = 2; i < n - 1 && len < sizeof(payload); i++) {
            uint8_t c = f[i];
            if (esc) {
                payload[len++] = c == KISS_TFEND ? KISS_FEND : c == KISS_TFESC ? KISS_FESC : c;
                esc = false;
            } else if (c == KISS_FESC) {
                esc = true;
            } else {
                payload[len++] = c;
            }
        }
        handle_sethw(payload, len);
        return;
    }
    const uint8_t cmd = f[1];
    if (cmd == 0xFF) {
        ESP_LOGW(TAG, "app sent KISS exit; ignored (the adapter manages KISS mode)");
        return;
    }
    const uint8_t type = cmd & 0x0F;
    if (type == 0x01 && n >= 4) {
        s_txdelay_ms = (uint32_t)f[2] * 10; /* KISS TXDELAY is in 10 ms units */
    }
    if (type >= 1 && type <= 5 && n <= KISS_PARAM_MAXLEN) {
        /* Channel-access parameter: keep it if KISS isn't up yet. */
        xSemaphoreTake(s_param_lock, portMAX_DELAY);
        if (!s_kiss_active) {
            s_params[type].len = n;
            memcpy(s_params[type].data, f, n);
            xSemaphoreGive(s_param_lock);
            return;
        }
        xSemaphoreGive(s_param_lock);
    } else if (!s_kiss_active) {
        post_arg(EV_KISS_NEEDED, 1); /* data needs the TNC; bring KISS up now */
    }
    if (forward_to_radio(f, n) && type == 0x00) {
        status_led_tx(estimate_tx_ms(n));
    }
}

/* Reassemble KISS frames from the app's byte stream (frames may span writes). */
static void app_tx_task(void *arg)
{
    (void)arg;
    static uint8_t frame[KISS_FRAME_MAX];
    size_t flen = 0; /* frame[0] is the opening FEND when flen > 0 */
    uint8_t buf[256];
    for (;;) {
        size_t n = xStreamBufferReceive(s_app_tx, buf, sizeof(buf), portMAX_DELAY);
        for (size_t i = 0; i < n; i++) {
            const uint8_t c = buf[i];
            if (c == KISS_FEND) {
                if (flen > 1) {
                    frame[flen++] = KISS_FEND;
                    dispatch_frame(frame, flen);
                }
                frame[0] = KISS_FEND; /* closing FEND doubles as the next opening */
                flen = 1;
            } else if (flen == 0) {
                forward_to_radio(&c, 1); /* stray byte outside any frame */
            } else if (flen < sizeof(frame) - 1) {
                frame[flen++] = c;
            } else {
                ESP_LOGW(TAG, "KISS frame too long; passing through unparsed");
                forward_to_radio(frame, flen);
                flen = 0;
            }
        }
    }
}

static void grace_timer_cb(TimerHandle_t t)
{
    (void)t;
    post_arg(EV_KISS_NEEDED, 0);
}

esp_err_t bridge_start(const char *ble_name)
{
    s_events = xQueueCreate(16, sizeof(bridge_msg_t));
    s_flags = xEventGroupCreate();
    s_app_tx = xStreamBufferCreate(APP_TX_BUF_SIZE, 1);
    s_param_lock = xSemaphoreCreateMutex();
    s_to_app_lock = xSemaphoreCreateMutex();
    s_grace_timer = xTimerCreate("kiss_grace", pdMS_TO_TICKS(KISS_GRACE_MS), pdFALSE, NULL,
                                 grace_timer_cb);
    ESP_RETURN_ON_FALSE(s_events && s_flags && s_app_tx && s_param_lock && s_to_app_lock &&
                        s_grace_timer,
                        ESP_ERR_NO_MEM, TAG, "no memory");
    ESP_RETURN_ON_ERROR(cat_init(), TAG, "cat_init");
    if (status_led_start() != ESP_OK) {
        ESP_LOGW(TAG, "status LED not available; continuing without it");
    }

    BaseType_t ok = xTaskCreate(control_task, "bridge", 4096, NULL, 6, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "control task");
    ok = xTaskCreate(app_tx_task, "app_tx", 3072, NULL, 6, NULL);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "app_tx task");

    const radio_usb_config_t usb_config = {
        .rx_cb = on_radio_rx,
        .state_cb = on_radio_state,
    };
    ESP_RETURN_ON_ERROR(radio_usb_start(&usb_config), TAG, "radio_usb_start");

    const ble_kiss_config_t ble_config = {
        .device_name = ble_name,
        .capabilities = BLE_KISS_CAP_QSY, /* 0xEA/0xEB are handled (translated to CAT) */
        .rx_cb = on_app_rx,
        .session_cb = on_app_session,
    };
    return ble_kiss_start(&ble_config);
}
