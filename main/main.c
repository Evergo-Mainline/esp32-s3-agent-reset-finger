/*
 * SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Evergo-Mainline
 *
 * evergo reboot helper - ESP32-S3 Super Mini
 *
 * Drives the phone's power / volume-down / volume-up buttons through
 * open-drain MOSFETs and exposes an HTTP API over WiFi.
 *
 * The API is designed to be just as usable from a script or an AI agent as
 * from the web page:
 *
 *   GET  /                 web console
 *   GET  /api              self-describing JSON (actions, params, ranges)
 *   GET  /status           state + busy info + last action result
 *   GET  /<action>         run an action, params in the query string
 *   POST /api              run an action, JSON body (recommended for AI)
 *
 * Actions: ping, on, off, reboot, fastboot, power, voldown, volup.
 * Only one action runs at a time; a conflicting request gets 409 with
 * busy_for_ms/retry_after_ms. A request_id makes retries idempotent for 30 s.
 * wait=1 blocks until the action finishes (max 30 s).
 *
 * Wiring (both GNDs must be joined - the USB cable does it):
 *   GPIO4 -> 1k -> 2N7002 gate, drain -> phone POWER button line
 *   GPIO5 -> 1k -> 2N7002 gate, drain -> phone VOL DOWN line
 *   GPIO6 -> 1k -> 2N7002 gate, drain -> phone VOL UP line
 *   source of every MOSFET -> GND
 *
 * The MOSFET inverts: GPIO high pulls the button line low, i.e. "pressed".
 * Never drive a voltage into the phone's button lines - they sit on a 1.8 V
 * rail and only tolerate being shorted to ground.
 *
 * The status page is web/index.html; main/web_page.h is generated from it by
 * tools/gen_web_page.py and must not be edited by hand.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"

#include "json_flat.h"
#include "web_page.h"
#include "wifi_config.h"

static const char *TAG = "helper";

#define API_VERSION      "2.0"
#define WAIT_MAX_MS      30000   /* max time wait=1 blocks the HTTP task */
#define IDEM_TTL_MS      30000   /* request_id replay window */
#define IDEM_SLOTS       8       /* request ids remembered */
#define IDEM_ID_MAX      41      /* 40 chars + NUL */
#define HTTP_BODY_MAX    512

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void copy_str(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src);
}

/* --------------------------------------------------------------- buttons -- */
#define GPIO_BTN_POWER    GPIO_NUM_4
#define GPIO_BTN_VOLDOWN  GPIO_NUM_5
#define GPIO_BTN_VOLUP    GPIO_NUM_6

#define PRESS_SHORT_MS    300
#define HOLD_OFF_MS       10000
#define BOOT_WAIT_MS      3000
#define FASTBOOT_HOLD_MS  2000

/* open-drain button helpers: low = pressed, high = released */
#define BTN_DOWN(p)  gpio_set_level((p), 0)
#define BTN_UP(p)    gpio_set_level((p), 1)

static void btn_init(void)
{
    const gpio_num_t pins[] = { GPIO_BTN_POWER, GPIO_BTN_VOLDOWN, GPIO_BTN_VOLUP };
    for (int i = 0; i < 3; i++) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << pins[i],
            /* open drain: level 1 releases the line, level 0 pulls it to GND */
            .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        BTN_UP(pins[i]);
    }
}

static void press(gpio_num_t pin, uint32_t ms)
{
    ESP_LOGI(TAG, "press gpio%d for %u ms", pin, (unsigned)ms);
    BTN_DOWN(pin);
    vTaskDelay(pdMS_TO_TICKS(ms));
    BTN_UP(pin);
}

static void do_reboot_phone(void)
{
    ESP_LOGW(TAG, "forcing power off (hold %d ms)", HOLD_OFF_MS);
    press(GPIO_BTN_POWER, HOLD_OFF_MS);
    vTaskDelay(pdMS_TO_TICKS(BOOT_WAIT_MS));
    ESP_LOGW(TAG, "powering on");
    press(GPIO_BTN_POWER, PRESS_SHORT_MS);
}

static void do_fastboot(uint32_t power_ms, uint32_t hold_ms, bool force_off)
{
    if (force_off) {
        ESP_LOGW(TAG, "fastboot: forcing power off");
        press(GPIO_BTN_POWER, HOLD_OFF_MS);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    /* POWER + VOL-DOWN together, POWER released after power_ms, VOL-DOWN kept
     * down for hold_ms more. */
    ESP_LOGW(TAG, "fastboot: combo t=0 power+voldown");
    BTN_DOWN(GPIO_BTN_POWER);
    BTN_DOWN(GPIO_BTN_VOLDOWN);

    vTaskDelay(pdMS_TO_TICKS(power_ms));
    ESP_LOGW(TAG, "fastboot: t=%u release power, keep voldown",
             (unsigned)power_ms);
    BTN_UP(GPIO_BTN_POWER);

    vTaskDelay(pdMS_TO_TICKS(hold_ms));
    ESP_LOGW(TAG, "fastboot: t=%u release voldown",
             (unsigned)(power_ms + hold_ms));
    BTN_UP(GPIO_BTN_VOLDOWN);

    ESP_LOGW(TAG, "fastboot: sequence done");
}

/* ------------------------------------------------------------------ led ---- */
/* GPIO48 drives a WS2812 addressable RGB LED.  It needs the RMT peripheral:
 * 10 MHz resolution, T0H/T0L = 3/9 ticks, T1H/T1L = 9/3, GRB byte order. */
#define LED_GPIO        GPIO_NUM_48
#define LED_RES_HZ      10000000       /* 0.1 us per tick */

static rmt_channel_handle_t s_led_chan;
static rmt_encoder_handle_t s_led_enc;

static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_led_chan || !s_led_enc) {
        return;
    }
    uint8_t grb[3] = { g, r, b };          /* WS2812 wants G R B */
    rmt_transmit_config_t tx = { .loop_count = 0 };
    rmt_transmit(s_led_chan, s_led_enc, grb, sizeof(grb), &tx);
    rmt_tx_wait_all_done(s_led_chan, 100);
    esp_rom_delay_us(80);                  /* > 50 us low latches the frame */
}

/* Two flashes in the given colour. */
static void led_ack(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < 2; i++) {
        led_set(r, g, b);
        vTaskDelay(pdMS_TO_TICKS(45));
        led_set(0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(45));
    }
}

static void led_init(void)
{
    rmt_tx_channel_config_t tx = {
        .gpio_num          = LED_GPIO,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = LED_RES_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
    };
    if (rmt_new_tx_channel(&tx, &s_led_chan) != ESP_OK) {
        ESP_LOGW(TAG, "led: no rmt channel");
        return;
    }

    rmt_bytes_encoder_config_t enc = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };
    if (rmt_new_bytes_encoder(&enc, &s_led_enc) != ESP_OK) {
        ESP_LOGW(TAG, "led: no encoder");
        return;
    }
    rmt_enable(s_led_chan);
    led_set(0, 0, 0);
}

/* ------------------------------------------------------------------ wifi -- */
static EventGroupHandle_t s_wifi_events;
#define WIFI_CONNECTED_BIT BIT0

static volatile bool s_auto_connect;
static char s_ip[32] = "0.0.0.0";
static char s_ssid[33] = "";

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_auto_connect) {
            ESP_LOGW(TAG, "disconnected, retrying");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        /* the connection may complete asynchronously, so read back the name */
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(s_ssid, sizeof(s_ssid), "%s", (char *)ap.ssid);
        }
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "got ip %s", s_ip);
    }
}

static void wifi_scan_and_log(void)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        return;
    }
    uint16_t n = 12;
    wifi_ap_record_t aps[12];
    if (esp_wifi_scan_get_ap_records(&n, aps) != ESP_OK) {
        return;
    }
    ESP_LOGW(TAG, "visible access points (%u):", n);
    for (int i = 0; i < n; i++) {
        ESP_LOGW(TAG, "  %-24s ch%-3d rssi%-4d auth%d",
                 (char *)aps[i].ssid, aps[i].primary, aps[i].rssi,
                 aps[i].authmode);
    }
}

#define TRY_CALL(x) do {                                        \
        esp_err_t e_ = (x);                                     \
        if (e_ != ESP_OK) {                                     \
            ESP_LOGE(TAG, "%s -> %s", #x, esp_err_to_name(e_));  \
            return;                                             \
        }                                                       \
    } while (0)

static void wifi_start(void)
{
    s_wifi_events = xEventGroupCreate();

    TRY_CALL(esp_netif_init());
    TRY_CALL(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    TRY_CALL(esp_wifi_init(&init));

    TRY_CALL(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    TRY_CALL(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    const char *candidates[] = {
        WIFI_SSID_PRIMARY, WIFI_SSID_ALT1, WIFI_SSID_ALT2
    };

    TRY_CALL(esp_wifi_set_mode(WIFI_MODE_STA));
    TRY_CALL(esp_wifi_start());
    /* no modem sleep: the page should react immediately to a tap */
    esp_wifi_set_ps(WIFI_PS_NONE);

    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        wifi_config_t cfg = { 0 };
        strncpy((char *)cfg.sta.ssid, candidates[i], sizeof(cfg.sta.ssid) - 1);
        strncpy((char *)cfg.sta.password, WIFI_PASSWORD,
                sizeof(cfg.sta.password) - 1);
        /* accept WPA2 and WPA3 so the same firmware works on either */
        cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        cfg.sta.pmf_cfg.capable = true;
        cfg.sta.pmf_cfg.required = false;

        ESP_LOGI(TAG, "trying ssid '%s'", candidates[i]);
        /* the interface must be idle before its config can be replaced */
        s_auto_connect = false;
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));

        esp_err_t sc = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (sc != ESP_OK) {
            ESP_LOGE(TAG, "set_config(%s) -> %s", candidates[i],
                     esp_err_to_name(sc));
            continue;
        }
        s_auto_connect = true;
        esp_wifi_connect();

        EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                               pdFALSE, pdTRUE,
                                               pdMS_TO_TICKS(10000));
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "connected to '%s'", s_ssid);
            return;
        }
        ESP_LOGW(TAG, "'%s' failed", candidates[i]);
    }

    wifi_scan_and_log();
    ESP_LOGE(TAG, "no configured ssid worked - check wifi_config.h");
}

/* --------------------------------------------------------------- actions -- */
typedef enum {
    ACT_PING, ACT_ON, ACT_OFF, ACT_REBOOT, ACT_FASTBOOT,
    ACT_POWER, ACT_VOLDOWN, ACT_VOLUP, ACT_COUNT
} action_id_t;

typedef struct {
    const char *name;
} action_meta_t;

static const action_meta_t ACTION_META[ACT_COUNT] = {
    [ACT_PING]     = { "ping" },
    [ACT_ON]       = { "on" },
    [ACT_OFF]      = { "off" },
    [ACT_REBOOT]   = { "reboot" },
    [ACT_FASTBOOT] = { "fastboot" },
    [ACT_POWER]    = { "power" },
    [ACT_VOLDOWN]  = { "voldown" },
    [ACT_VOLUP]    = { "volup" },
};

typedef struct {
    action_id_t id;
    uint32_t    ms;            /* press duration / ping delay */
    uint32_t    ms2;           /* extra hold, fastboot       */
    bool        off;           /* force power off first      */
    uint32_t    started_ms;    /* queue accepted at           */
    char        request_id[IDEM_ID_MAX];
} action_t;

typedef struct {
    bool     valid;
    int      id;
    uint32_t elapsed_ms;
    uint32_t done_ms;
    char     request_id[IDEM_ID_MAX];
} last_action_t;

static QueueHandle_t s_actions;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_busy;
static volatile int s_busy_id = ACT_COUNT;
static volatile uint32_t s_busy_start_ms;
static volatile uint32_t s_busy_eta_ms;
static last_action_t s_last;

static bool is_valid_request_id(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n > 40) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' ||
              c == ':')) {
            return false;
        }
    }
    return true;
}

/* Rough duration of an action, used for retry_after_ms hints. */
static uint32_t action_eta(const action_t *a)
{
    switch (a->id) {
    case ACT_ON:       return PRESS_SHORT_MS;
    case ACT_OFF:      return HOLD_OFF_MS;
    case ACT_REBOOT:   return HOLD_OFF_MS + BOOT_WAIT_MS + PRESS_SHORT_MS;
    case ACT_FASTBOOT: return (a->off ? HOLD_OFF_MS + 2000u : 0u) + a->ms + a->ms2;
    case ACT_PING:     return a->ms;
    default:           return a->ms;
    }
}

static void execute_action(const action_t *a)
{
    switch (a->id) {
    case ACT_PING:     vTaskDelay(pdMS_TO_TICKS(a->ms));      break;
    case ACT_ON:       press(GPIO_BTN_POWER, PRESS_SHORT_MS); break;
    case ACT_OFF:      press(GPIO_BTN_POWER, HOLD_OFF_MS);    break;
    case ACT_REBOOT:   do_reboot_phone();                     break;
    case ACT_FASTBOOT: do_fastboot(a->ms, a->ms2, a->off);    break;
    case ACT_POWER:    press(GPIO_BTN_POWER, a->ms);          break;
    case ACT_VOLDOWN:  press(GPIO_BTN_VOLDOWN, a->ms);        break;
    case ACT_VOLUP:    press(GPIO_BTN_VOLUP, a->ms);          break;
    default: break;
    }
}

static void action_led(action_id_t id, uint8_t *r, uint8_t *g, uint8_t *b)
{
    switch (id) {
    case ACT_ON:       *r = 0;  *g = 45; *b = 0;  break;  /* green  */
    case ACT_OFF:      *r = 45; *g = 0;  *b = 0;  break;  /* red    */
    case ACT_REBOOT:   *r = 45; *g = 25; *b = 0;  break;  /* orange */
    case ACT_FASTBOOT: *r = 0;  *g = 0;  *b = 55; break;  /* blue   */
    case ACT_PING:     *r = 0;  *g = 0;  *b = 0;  break;  /* silent */
    default:           *r = 12; *g = 12; *b = 12; break;  /* white  */
    }
}

static void action_task(void *arg)
{
    action_t a;
    for (;;) {
        if (xQueueReceive(s_actions, &a, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        ESP_LOGI(TAG, "action: %s", ACTION_META[a.id].name);
        execute_action(&a);

        uint32_t now = uptime_ms();
        portENTER_CRITICAL(&s_mux);
        s_last.valid = true;
        s_last.id = a.id;
        s_last.elapsed_ms = now - a.started_ms;
        s_last.done_ms = now;
        memcpy(s_last.request_id, a.request_id, sizeof(s_last.request_id));
        s_busy = false;
        s_busy_id = ACT_COUNT;
        portEXIT_CRITICAL(&s_mux);
    }
}

/* Only one button action may run at a time: reject instead of queueing, so a
 * second request can never interleave half-finished sequences. */
static bool queue_action(action_t a)
{
    uint32_t now = uptime_ms();
    a.started_ms = now;

    portENTER_CRITICAL(&s_mux);
    bool free_slot = !s_busy;
    if (free_slot) {
        s_busy = true;
        s_busy_id = a.id;
        s_busy_start_ms = now;
        s_busy_eta_ms = action_eta(&a);
    }
    portEXIT_CRITICAL(&s_mux);

    if (!free_slot) {
        return false;
    }
    if (xQueueSend(s_actions, &a, 0) == pdTRUE) {
        return true;
    }

    portENTER_CRITICAL(&s_mux);
    s_busy = false;
    s_busy_id = ACT_COUNT;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGE(TAG, "action queue unexpectedly full");
    return false;
}

static bool action_state(action_id_t *id, uint32_t *busy_for_ms,
                         uint32_t *retry_after_ms)
{
    portENTER_CRITICAL(&s_mux);
    bool busy = s_busy;
    int idv = s_busy_id;
    uint32_t start = s_busy_start_ms;
    uint32_t eta = s_busy_eta_ms;
    portEXIT_CRITICAL(&s_mux);

    if (id != NULL) {
        *id = (action_id_t)idv;
    }
    uint32_t elapsed = busy ? (uptime_ms() - start) : 0;
    if (busy_for_ms != NULL) {
        *busy_for_ms = elapsed;
    }
    if (retry_after_ms != NULL) {
        *retry_after_ms = busy ? (eta > elapsed ? eta - elapsed + 300 : 500) : 300;
    }
    return busy;
}

static bool last_snapshot(last_action_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_last;
    portEXIT_CRITICAL(&s_mux);
    return out->valid;
}

/* Wait until our action is done. Returns false on timeout. */
static bool wait_for_action(action_id_t id, uint32_t max_ms)
{
    uint32_t t0 = uptime_ms();
    for (;;) {
        last_action_t last;
        bool has_last = last_snapshot(&last);
        if (has_last && last.id == (int)id && (int32_t)(last.done_ms - t0) >= 0) {
            return true;
        }

        action_id_t busy_id;
        uint32_t busy_for, retry;
        if (!action_state(&busy_id, &busy_for, &retry)) {
            return has_last && last.id == (int)id;
        }
        if (uptime_ms() - t0 >= max_ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
}

static uint32_t action_elapsed(action_id_t id)
{
    last_action_t last;
    if (last_snapshot(&last) && last.id == (int)id) {
        return last.elapsed_ms;
    }
    return 0;
}

/* --------------------------------------------------- request_id idempotency */
typedef struct {
    bool     used;
    char     id[IDEM_ID_MAX];
    char     response[224];
    uint32_t at_ms;
} idem_entry_t;

static idem_entry_t s_idem[IDEM_SLOTS];
static int s_idem_next;

static const char *idem_lookup(const char *id)
{
    uint32_t now = uptime_ms();
    for (int i = 0; i < IDEM_SLOTS; i++) {
        if (!s_idem[i].used || strcmp(s_idem[i].id, id) != 0) {
            continue;
        }
        if (now - s_idem[i].at_ms <= IDEM_TTL_MS) {
            return s_idem[i].response;
        }
        s_idem[i].used = false;
        return NULL;
    }
    return NULL;
}

static void idem_store(const char *id, const char *response)
{
    idem_entry_t *e = &s_idem[s_idem_next];
    s_idem_next = (s_idem_next + 1) % IDEM_SLOTS;
    e->used = true;
    copy_str(e->id, sizeof(e->id), id);
    copy_str(e->response, sizeof(e->response), response);
    e->at_ms = uptime_ms();
}

/* ------------------------------------------------------------------ http -- */
static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static void json_escape(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; *src != '\0' && n + 2 < cap; src++) {
        unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            dst[n++] = '\\';
            dst[n++] = (char)c;
        } else if (c < 0x20) {
            dst[n++] = ' ';
        } else {
            dst[n++] = (char)c;
        }
    }
    dst[n] = '\0';
}

static esp_err_t send_error(httpd_req_t *req, const char *status,
                            const char *code, const char *message)
{
    char esc[128];
    json_escape(esc, sizeof(esc), message);

    char buf[224];
    snprintf(buf, sizeof(buf),
             "{\"ok\":false,\"error\":\"%s\",\"message\":\"%s\","
             "\"hint\":\"GET /api\"}", code, esc);
    httpd_resp_set_status(req, status);
    return send_json(req, buf);
}

static esp_err_t send_conflict(httpd_req_t *req)
{
    action_id_t id = ACT_COUNT;
    uint32_t busy_for_ms = 0, retry_after_ms = 300;
    action_state(&id, &busy_for_ms, &retry_after_ms);

    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"ok\":false,\"error\":\"action_in_progress\","
             "\"action\":\"%s\",\"busy_for_ms\":%u,"
             "\"retry_after_ms\":%u}",
             (id < ACT_COUNT) ? ACTION_META[id].name : "",
             (unsigned)busy_for_ms, (unsigned)retry_after_ms);

    char retry_s[12];
    snprintf(retry_s, sizeof(retry_s), "%u",
             (unsigned)((retry_after_ms + 999u) / 1000u));
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_hdr(req, "Retry-After", retry_s);
    return send_json(req, buf);
}

/* int query parameter, validated against a range. */
static bool query_int(httpd_req_t *req, const char *key,
                      long min, long max, long *out)
{
    char q[160], v[12];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) {
        return false;
    }
    long n = strtol(v, NULL, 10);
    if (n < min || n > max) {
        return false;
    }
    *out = n;
    return true;
}

static bool query_str(httpd_req_t *req, const char *key,
                      char *out, size_t out_len)
{
    char q[192], v[64];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) {
        return false;
    }
    copy_str(out, out_len, v);
    return true;
}

static uint32_t query_ms(httpd_req_t *req, uint32_t def)
{
    long ms;
    return query_int(req, "ms", 20, 15000, &ms) ? (uint32_t)ms : def;
}

static bool query_wait(httpd_req_t *req)
{
    long v;
    return query_int(req, "wait", 0, 1, &v) && v != 0;
}

/* Run one action and build the response. `request_id` may be NULL. */
static esp_err_t start_action(httpd_req_t *req, action_id_t id,
                              uint32_t ms, uint32_t ms2, bool off,
                              const char *request_id, bool wait)
{
    char rid[IDEM_ID_MAX] = "";
    if (request_id != NULL && request_id[0] != '\0') {
        if (!is_valid_request_id(request_id)) {
            return send_error(req, "400 Bad Request", "invalid_request_id",
                              "request_id must be 1-40 chars of [A-Za-z0-9._:-]");
        }
        copy_str(rid, sizeof(rid), request_id);
        const char *cached = idem_lookup(rid);
        if (cached != NULL) {
            httpd_resp_set_hdr(req, "X-Idempotent-Replay", "1");
            return send_json(req, cached);
        }
    }

    action_t a = { .id = id, .ms = ms, .ms2 = ms2, .off = off };
    copy_str(a.request_id, sizeof(a.request_id), rid);
    if (!queue_action(a)) {
        return send_conflict(req);
    }

    uint8_t r, g, b;
    action_led(id, &r, &g, &b);
    led_ack(r, g, b);

    char idfrag[IDEM_ID_MAX + 24];
    if (rid[0] != '\0') {
        snprintf(idfrag, sizeof(idfrag), ",\"request_id\":\"%s\"", rid);
    } else {
        idfrag[0] = '\0';
    }

    char resp[224];
    if (!wait) {
        snprintf(resp, sizeof(resp),
                 "{\"ok\":true,\"action\":\"%s\",\"accepted\":true%s}",
                 ACTION_META[id].name, idfrag);
    } else {
        bool done = wait_for_action(id, WAIT_MAX_MS);
        if (done) {
            snprintf(resp, sizeof(resp),
                     "{\"ok\":true,\"action\":\"%s\",\"accepted\":true,"
                     "\"done\":true,\"elapsed_ms\":%u%s}",
                     ACTION_META[id].name,
                     (unsigned)action_elapsed(id), idfrag);
        } else {
            snprintf(resp, sizeof(resp),
                     "{\"ok\":true,\"action\":\"%s\",\"accepted\":true,"
                     "\"done\":false,\"pending\":true%s}",
                     ACTION_META[id].name, idfrag);
        }
    }

    if (rid[0] != '\0') {
        idem_store(rid, resp);
    }
    return send_json(req, resp);
}

/* GET action handlers: query params only. */
static esp_err_t action_from_query(httpd_req_t *req, action_id_t id,
                                   uint32_t ms, uint32_t ms2, bool off)
{
    char rid[IDEM_ID_MAX] = "";
    query_str(req, "request_id", rid, sizeof(rid));
    return start_action(req, id, ms, ms2, off,
                        rid[0] != '\0' ? rid : NULL, query_wait(req));
}

static esp_err_t h_ping(httpd_req_t *req)
{
    long ms = 0;
    query_int(req, "ms", 0, 15000, &ms);
    return action_from_query(req, ACT_PING, (uint32_t)ms, 0, false);
}

static esp_err_t h_on(httpd_req_t *req)
{
    return action_from_query(req, ACT_ON, 0, 0, false);
}

static esp_err_t h_off(httpd_req_t *req)
{
    return action_from_query(req, ACT_OFF, 0, 0, false);
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    return action_from_query(req, ACT_REBOOT, 0, 0, false);
}

static esp_err_t h_power(httpd_req_t *req)
{
    return action_from_query(req, ACT_POWER, query_ms(req, PRESS_SHORT_MS), 0, false);
}

static esp_err_t h_voldown(httpd_req_t *req)
{
    return action_from_query(req, ACT_VOLDOWN, query_ms(req, PRESS_SHORT_MS), 0, false);
}

static esp_err_t h_volup(httpd_req_t *req)
{
    return action_from_query(req, ACT_VOLUP, query_ms(req, PRESS_SHORT_MS), 0, false);
}

static esp_err_t h_fastboot(httpd_req_t *req)
{
    long v;
    uint32_t power_ms = 4000;
    uint32_t hold_ms = 1000;
    bool off = true;

    if (query_int(req, "power", 500, 10000, &v))  power_ms = (uint32_t)v;
    if (query_int(req, "hold", 100, 10000, &v))   hold_ms = (uint32_t)v;
    if (query_int(req, "off", 0, 1, &v))          off = v != 0;

    return action_from_query(req, ACT_FASTBOOT, power_ms, hold_ms, off);
}

/* POST /api: JSON body, flat object. */
static bool json_int_param(const jflat_t *j, const char *key, long min, long max,
                           long *value, char *err, size_t err_len)
{
    if (jflat_find(j, key) == NULL) {
        return true;                     /* absent: caller keeps its default */
    }
    long v;
    if (!jflat_int(j, key, &v)) {
        snprintf(err, err_len, "%s must be an integer", key);
        return false;
    }
    if (v < min || v > max) {
        snprintf(err, err_len, "%s must be in [%ld, %ld]", key, min, max);
        return false;
    }
    *value = v;
    return true;
}

static esp_err_t h_api_post(httpd_req_t *req)
{
    char body[HTTP_BODY_MAX];
    int total = 0;
    for (;;) {
        if (total >= (int)sizeof(body) - 1) {
            return send_error(req, "413 Payload Too Large", "body_too_large",
                              "request body must be smaller than 512 bytes");
        }
        int r = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            return send_error(req, "408 Request Timeout", "timeout",
                              "request body not received in time");
        }
        if (r <= 0) {
            break;
        }
        total += r;
    }
    body[total] = '\0';

    jflat_t j;
    if (!jflat_parse(body, &j)) {
        return send_error(req, "400 Bad Request", "bad_json",
                          "body must be one flat JSON object");
    }

    const char *action_name;
    if (!jflat_str(&j, "action", &action_name)) {
        return send_error(req, "400 Bad Request", "missing_action",
                          "field \"action\" is required; see GET /api");
    }
    int id = -1;
    for (int i = 0; i < ACT_COUNT; i++) {
        if (strcmp(ACTION_META[i].name, action_name) == 0) {
            id = i;
            break;
        }
    }
    if (id < 0) {
        return send_error(req, "400 Bad Request", "unknown_action",
                          "unknown action; see GET /api");
    }

    const char *request_id = NULL;
    jflat_str(&j, "request_id", &request_id);
    if (request_id != NULL && !is_valid_request_id(request_id)) {
        return send_error(req, "400 Bad Request", "invalid_request_id",
                          "request_id must be 1-40 chars of [A-Za-z0-9._:-]");
    }

    bool wait = false;
    if (jflat_find(&j, "wait") != NULL && !jflat_bool(&j, "wait", &wait)) {
        return send_error(req, "400 Bad Request", "invalid_param",
                          "wait must be true or false");
    }

    char err[96];
    long v;
    uint32_t ms = 0;
    uint32_t ms2 = 0;
    bool off = false;

    switch ((action_id_t)id) {
    case ACT_PING:
        if (!json_int_param(&j, "ms", 0, 15000, &v, err, sizeof(err))) {
            return send_error(req, "400 Bad Request", "invalid_param", err);
        }
        ms = (uint32_t)v;
        break;
    case ACT_POWER:
    case ACT_VOLDOWN:
    case ACT_VOLUP:
        v = PRESS_SHORT_MS;
        if (!json_int_param(&j, "ms", 20, 15000, &v, err, sizeof(err))) {
            return send_error(req, "400 Bad Request", "invalid_param", err);
        }
        ms = (uint32_t)v;
        break;
    case ACT_FASTBOOT:
        v = 4000;
        if (!json_int_param(&j, "power", 500, 10000, &v, err, sizeof(err))) {
            return send_error(req, "400 Bad Request", "invalid_param", err);
        }
        ms = (uint32_t)v;
        v = 1000;
        if (!json_int_param(&j, "hold", 100, 10000, &v, err, sizeof(err))) {
            return send_error(req, "400 Bad Request", "invalid_param", err);
        }
        ms2 = (uint32_t)v;
        if (jflat_find(&j, "off") != NULL) {
            if (!jflat_bool(&j, "off", &off)) {
                return send_error(req, "400 Bad Request", "invalid_param",
                                  "off must be true or false");
            }
        } else {
            off = true;
        }
        break;
    default:
        break;
    }

    return start_action(req, (action_id_t)id, ms, ms2, off, request_id, wait);
}

/* GET /api: self-describing document, so an agent can discover everything
 * without reading the source. */
static const char API_DOC[] =
    "{"
      "\"name\":\"evergo reboot helper\","
      "\"version\":\"" API_VERSION "\","
      "\"device\":\"ESP32-S3 Super Mini (2.4GHz WiFi)\","
      "\"description\":\"用 WiFi 控制手机电源/音量按键的 HTTP 接口。同一时间只允许一个动作。\","
      "\"endpoints\":{"
        "\"GET /\":\"网页控制台\","
        "\"GET /api\":\"本自描述文档\","
        "\"GET /status\":\"设备状态、忙闲信息和最近一次动作结果\","
        "\"GET /<action>\":\"执行动作，参数用查询串传入\","
        "\"POST /api\":\"执行动作，JSON 请求体（推荐 AI 使用）\""
      "},"
      "\"actions\":{"
        "\"ping\":{\"description\":\"连通性测试，不操作手机\",\"params\":{\"ms\":{\"type\":\"int\",\"min\":0,\"max\":15000,\"default\":0,\"unit\":\"ms\"}}},"
        "\"on\":{\"description\":\"短按电源键，开机\"},"
        "\"off\":{\"description\":\"长按电源键 10 秒，强制关机\"},"
        "\"reboot\":{\"description\":\"强制关机，等待 3 秒后短按电源，重启手机\"},"
        "\"fastboot\":{\"description\":\"电源键+音量减组合键进入 fastboot\",\"params\":{"
          "\"power\":{\"type\":\"int\",\"min\":500,\"max\":10000,\"default\":4000,\"unit\":\"ms\"},"
          "\"hold\":{\"type\":\"int\",\"min\":100,\"max\":10000,\"default\":1000,\"unit\":\"ms\"},"
          "\"off\":{\"type\":\"bool\",\"default\":true}}},"
        "\"power\":{\"description\":\"按电源键指定时长\",\"params\":{\"ms\":{\"type\":\"int\",\"min\":20,\"max\":15000,\"default\":300,\"unit\":\"ms\"}}},"
        "\"voldown\":{\"description\":\"按音量减键指定时长\",\"params\":{\"ms\":{\"type\":\"int\",\"min\":20,\"max\":15000,\"default\":300,\"unit\":\"ms\"}}},"
        "\"volup\":{\"description\":\"按音量加键指定时长\",\"params\":{\"ms\":{\"type\":\"int\",\"min\":20,\"max\":15000,\"default\":300,\"unit\":\"ms\"}}}"
      "},"
      "\"request\":{"
        "\"get_query\":\"ms / power / hold / off / wait=1 / request_id=<id>\","
        "\"post_body\":{\"action\":\"fastboot\",\"power\":4000,\"hold\":1000,\"off\":true,\"wait\":false,\"request_id\":\"<id>\"},"
        "\"request_id\":\"1-40 个 [A-Za-z0-9._:-] 字符。30 秒内相同 id 只执行一次，重复请求返回上次响应并带 X-Idempotent-Replay: 1\","
        "\"wait\":\"wait=1（GET）或 \\\"wait\\\":true（POST），最多阻塞 30 秒直到动作完成；等待期间 HTTP 服务被占用\""
      "},"
      "\"response\":{"
        "\"accepted\":{\"ok\":true,\"action\":\"reboot\",\"accepted\":true,\"request_id\":\"<id>\"},"
        "\"completed_wait\":{\"ok\":true,\"action\":\"reboot\",\"accepted\":true,\"done\":true,\"elapsed_ms\":13300},"
        "\"status\":{\"ssid\":\"MyWiFi\",\"ip\":\"192.168.1.23\",\"heap\":266000,\"uptime_s\":42,\"busy\":false,\"action\":\"\",\"busy_for_ms\":0,\"last\":{\"action\":\"reboot\",\"ok\":true,\"elapsed_ms\":13300,\"ago_ms\":5200,\"request_id\":\"<id>\"}},"
        "\"conflict_409\":{\"ok\":false,\"error\":\"action_in_progress\",\"action\":\"reboot\",\"busy_for_ms\":3200,\"retry_after_ms\":10100}"
      "},"
      "\"recommended_ai_flow\":["
        "\"1. GET /api 发现动作和参数\","
        "\"2. POST /api 执行动作，带上唯一 request_id\","
        "\"3. GET /status 的 last 字段（含 request_id）确认完成；或请求时加 wait=1 直接拿到结果\","
        "\"4. 收到 409 时按 retry_after_ms 等待后重试；相同 request_id 不会重复按键\""
      "],"
      "\"examples\":["
        "\"curl http://<ip>/api\","
        "\"curl http://<ip>/status\","
        "\"curl 'http://<ip>/ping?ms=500&wait=1'\","
        "\"curl -X POST http://<ip>/api -H 'Content-Type: application/json' -d '{\\\"action\\\":\\\"reboot\\\",\\\"request_id\\\":\\\"boot-001\\\"}'\""
      "]"
    "}";

static esp_err_t h_api(httpd_req_t *req)
{
    return send_json(req, API_DOC);
}

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_status(httpd_req_t *req)
{
    action_id_t id = ACT_COUNT;
    uint32_t busy_for_ms = 0, retry_after_ms = 0;
    bool busy = action_state(&id, &busy_for_ms, &retry_after_ms);

    last_action_t last;
    bool has_last = last_snapshot(&last);

    char ssid_esc[80];
    json_escape(ssid_esc, sizeof(ssid_esc), s_ssid);

    char last_json[192];
    if (has_last) {
        int last_id = (last.id >= 0 && last.id < ACT_COUNT) ? last.id : 0;
        snprintf(last_json, sizeof(last_json),
                 "{\"action\":\"%s\",\"ok\":true,\"elapsed_ms\":%u,"
                 "\"ago_ms\":%u,\"request_id\":\"%s\"}",
                 ACTION_META[last_id].name, (unsigned)last.elapsed_ms,
                 (unsigned)(uptime_ms() - last.done_ms), last.request_id);
    } else {
        copy_str(last_json, sizeof(last_json), "null");
    }

    char buf[448];
    snprintf(buf, sizeof(buf),
             "{\"ssid\":\"%s\",\"ip\":\"%s\",\"heap\":%u,\"uptime_s\":%u,"
             "\"busy\":%s,\"action\":\"%s\",\"busy_for_ms\":%u,\"last\":%s}",
             ssid_esc, s_ip, (unsigned)esp_get_free_heap_size(),
             (unsigned)(esp_timer_get_time() / 1000000),
             busy ? "true" : "false",
             (busy && id < ACT_COUNT) ? ACTION_META[id].name : "",
             (unsigned)busy_for_ms, last_json);
    return send_json(req, buf);
}

static esp_err_t h_options(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Max-Age", "600");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t h_not_found(httpd_req_t *req)
{
    return send_error(req, "404 Not Found", "not_found",
                      "no such endpoint; see GET /api");
}

static void http_start(void)
{
    httpd_handle_t srv = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.uri_match_fn = httpd_uri_match_wildcard;

    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }

    static const httpd_uri_t uris[] = {
        { "/",         HTTP_GET,     h_root,       NULL },
        { "/api",      HTTP_GET,     h_api,        NULL },
        { "/api",      HTTP_POST,    h_api_post,   NULL },
        { "/status",   HTTP_GET,     h_status,     NULL },
        { "/ping",     HTTP_GET,     h_ping,       NULL },
        { "/on",       HTTP_GET,     h_on,         NULL },
        { "/off",      HTTP_GET,     h_off,        NULL },
        { "/reboot",   HTTP_GET,     h_reboot,     NULL },
        { "/fastboot", HTTP_GET,     h_fastboot,   NULL },
        { "/power",    HTTP_GET,     h_power,      NULL },
        { "/voldown",  HTTP_GET,     h_voldown,    NULL },
        { "/volup",    HTTP_GET,     h_volup,      NULL },
        { "/*",        HTTP_GET,     h_not_found,  NULL },
        { "/*",        HTTP_POST,    h_not_found,  NULL },
        { "/*",        HTTP_OPTIONS, h_options,    NULL },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
    ESP_LOGI(TAG, "http server on port %d", cfg.server_port);
}

/* ------------------------------------------------------------------ main -- */
void app_main(void)
{
    ESP_LOGI(TAG, "evergo reboot helper starting (API v" API_VERSION ")");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    btn_init();
    led_init();
    s_actions = xQueueCreate(1, sizeof(action_t));
    xTaskCreate(action_task, "actions", 4096, NULL, 5, NULL);
    wifi_start();
    http_start();
    ESP_LOGI(TAG, "ready: http://%s/api", s_ip);
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive, ip %s, heap %u", s_ip,
                 (unsigned)esp_get_free_heap_size());
    }
}
