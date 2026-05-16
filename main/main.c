/*
 * main.c  -  NODE_1 Master (ESP32-S3)
 * Unified Project: Voice Recognition + ESP-NOW + WiFi + Blynk + Face++ + Dashboard
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"

/* ESP-SR API */
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_models.h"

#include "config.h"
#include "shared_types.h"
#include "wifi.h"
#include "espnow.h"
#include "http_client.h"
#include "face_verify.h"
#include "blynk.h"

/* Dashboard HTML */
extern const uint8_t dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const uint8_t dashboard_html_end[]   asm("_binary_dashboard_html_end");

static const char *TAG = "SENTINEL";

/* ================================================================
   VOICE REC / GPIO CONSTANTS
   ================================================================ */
#define I2S_WS_IO GPIO_NUM_4
#define I2S_BCLK_IO GPIO_NUM_5
#define I2S_DIN_IO GPIO_NUM_6

#define KITCHEN_LIGHT_PIN GPIO_NUM_7
#define BEDROOM_LIGHT_PIN GPIO_NUM_8
#define PARLOUR_LIGHT_PIN GPIO_NUM_9
#define OUTSIDE_LIGHT_PIN GPIO_NUM_10
#define COOLING_PIN GPIO_NUM_11

#define BEDROOM_SERVO_1_PIN GPIO_NUM_12
#define BEDROOM_SERVO_2_PIN GPIO_NUM_13
#define PARLOUR_SERVO_1_PIN GPIO_NUM_14
#define PARLOUR_SERVO_2_PIN GPIO_NUM_15

#define SERVO_FREQ_HZ 50
#define SERVO_TIMER_RES LEDC_TIMER_14_BIT
#define SERVO_DUTY_CLOSED 819
#define SERVO_DUTY_OPEN 1638

#define BEDROOM_SERVO_1_CH LEDC_CHANNEL_0
#define BEDROOM_SERVO_2_CH LEDC_CHANNEL_1
#define PARLOUR_SERVO_1_CH LEDC_CHANNEL_2
#define PARLOUR_SERVO_2_CH LEDC_CHANNEL_3

#define LOCKDOWN_PIN GPIO_NUM_16
#define BUZZER_PIN GPIO_NUM_17

#define MIN_COMMAND_CONFIDENCE 0.10f
#define CMD_COUNT 15

typedef struct {
  const char *phrase;
  int command_id;
} cmd_def_t;

static const cmd_def_t CMD_DEFS[CMD_COUNT] = {
    {"KITCHEN LIGHT ON", 1},    {"KITCHEN LIGHT OFF", 2},
    {"BEDROOM LIGHT ON", 3},    {"BEDROOM LIGHT OFF", 4},
    {"PARLOUR LIGHT ON", 5},    {"PARLOUR LIGHT OFF", 6},
    {"BEDROOM BLINDS OPEN", 7}, {"BEDROOM BLINDS CLOSE", 8},
    {"PARLOUR BLINDS OPEN", 9}, {"PARLOUR BLINDS CLOSE", 10},
    {"COOLING ON", 11},         {"COOLING OFF", 12},
    {"OUTSIDE LIGHT ON", 13},   {"OUTSIDE LIGHT OFF", 14},
    {"SENTINEL LOCKDOWN", 15},
};

static i2s_chan_handle_t rx_chan;
static esp_afe_sr_iface_t *g_afe_handle = NULL;
static esp_afe_sr_data_t *g_afe_data = NULL;

typedef enum { BEEP_SINGLE, BEEP_DOUBLE } beep_type_t;
static QueueHandle_t s_feedback_queue = NULL;

static bool bedroom_blinds_open = false;
static bool parlour_blinds_open = false;

/* ================================================================
   NETWORK / DEMO STATE
   ================================================================ */
static bool s_gas_was_danger = false;
static bool s_pir_was_active = false;
static SemaphoreHandle_t g_pipeline_mtx = NULL;

#define WEBHOOK_QUEUE_DEPTH  3
static QueueHandle_t g_webhook_queue = NULL;

#define DASH_EVENTS     5
#define NODE_TIMEOUT_MS 15000

typedef struct {
    char    t[8];
    int     lvl;
    char    msg[96];
} dash_event_t;

static struct {
    float lpg, ch4, smoke, h2;
    int gas_alert;
    int sec_alert;
    int pir;
    char face[80];
    uint32_t last_n2_ms;
    uint32_t last_n3_ms;
    uint32_t last_n4_ms;
    dash_event_t events[DASH_EVENTS];
    int          ev_count;
    int          ev_head;
} g_dash;

static SemaphoreHandle_t g_dash_mtx = NULL;

/* ----------------------------------------------------------------
 * DASHBOARD EVENTS
 * ---------------------------------------------------------------- */
static void dash_push_event(int lvl, const char *msg) {
    if (!g_dash_mtx) return;
    if (xSemaphoreTake(g_dash_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return;

    uint32_t s  = (xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000;
    uint32_t hh = (s / 3600) % 24;
    uint32_t mm = (s % 3600) / 60;

    dash_event_t *e = &g_dash.events[g_dash.ev_head];
    snprintf(e->t, sizeof(e->t), "%02lu:%02lu", (unsigned long)hh, (unsigned long)mm);
    e->lvl = lvl;
    strncpy(e->msg, msg ? msg : "", sizeof(e->msg) - 1);

    g_dash.ev_head = (g_dash.ev_head + 1) % DASH_EVENTS;
    if (g_dash.ev_count < DASH_EVENTS) g_dash.ev_count++;

    xSemaphoreGive(g_dash_mtx);
}

/* ================================================================
   FACE VERIFICATION PIPELINE
   ================================================================ */
static void face_pipeline(int trigger_location) {
    ESP_LOGI(TAG, "=== Pipeline start loc=%d (%s) ===",
             trigger_location, location_name(trigger_location));

    espnow_send_cmd(g_node4_mac, CMD_START_STREAM, 0, trigger_location);
    vTaskDelay(pdMS_TO_TICKS(2000));

    uint8_t  *jpeg = NULL;
    size_t    jlen = 0;
    esp_err_t ferr = ESP_FAIL;
    for (int i = 1; i <= 3 && ferr != ESP_OK; i++) {
        ferr = http_fetch_jpeg(&jpeg, &jlen);
        if (ferr != ESP_OK) {
            ESP_LOGW(TAG, "JPEG attempt %d/3 failed.", i);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (ferr != ESP_OK) {
        ESP_LOGE(TAG, "All JPEG attempts failed - aborting pipeline.");
        blynk_write_str(VPIN_FACE_RESULT, "Camera error");
        blynk_write_int(VPIN_SEC_ALERT, 1);
        espnow_send_cmd(g_node4_mac, CMD_STOP_STREAM, 0, 0);
        dash_push_event(2, "Camera error - JPEG fetch failed.");
        return;
    }

    char img_url[256] = {0};
    if (http_upload_imgbb(jpeg, jlen, img_url, sizeof(img_url)) == ESP_OK
        && img_url[0] != '\0') {
        blynk_set_property(VPIN_CAM_IMAGE, "url", img_url);
        ESP_LOGI(TAG, "Image URL pushed to V%d.", VPIN_CAM_IMAGE);
    }

    char  identity[32] = "Unknown";
    float confidence   = 0.0f;
    esp_err_t verr = face_verify(jpeg, jlen, identity, sizeof(identity), &confidence);

    heap_caps_free(jpeg);
    jpeg = NULL;

    if (verr != ESP_OK) {
        ESP_LOGE(TAG, "Face++ API error.");
        blynk_write_str(VPIN_FACE_RESULT, "API error");
        blynk_write_int(VPIN_SEC_ALERT, 1);
        espnow_send_cmd(g_node4_mac, CMD_STOP_STREAM, 0, 0);
        dash_push_event(2, "Face++ API error.");
        return;
    }

    int user_id = label_to_userid(identity);
    ESP_LOGI(TAG, "Identity: %s  uid=%d  conf=%.1f%%",
             identity, user_id, (double)confidence);

    if (xSemaphoreTake(g_dash_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
        snprintf(g_dash.face, sizeof(g_dash.face),
                 "%s|%.1f%%|%s",
                 identity, (double)confidence,
                 location_name(trigger_location));
        g_dash.sec_alert = (user_id > 0) ? 0 : 1;
        xSemaphoreGive(g_dash_mtx);
    }

    blynk_write_str(VPIN_FACE_RESULT, identity);
    blynk_write_int(VPIN_USER_ID,  user_id);
    blynk_write_int(VPIN_LOCATION, trigger_location);

    if (user_id > 0) {
        char event[80];
        snprintf(event, sizeof(event), "%s verified at %s",
                 identity, location_name(trigger_location));
        blynk_write_str(VPIN_EVENT_LABEL, event);
        blynk_write_int(VPIN_SEC_ALERT, 0);
        ESP_LOGI(TAG, "[GRANTED] %s (%.1f%%)", identity, (double)confidence);
        espnow_send_cmd(g_node2_mac, CMD_IDLE, user_id, trigger_location);
        dash_push_event(0, event);
    } else {
        char event[80];
        snprintf(event, sizeof(event),
                 "Unknown face at %s - DENIED (%.0f%%)",
                 location_name(trigger_location), (double)confidence);
        blynk_write_str(VPIN_EVENT_LABEL, event);
        blynk_write_int(VPIN_SEC_ALERT, 1);
        ESP_LOGW(TAG, "[DENIED] Unknown (conf=%.1f%%)", (double)confidence);
        dash_push_event(2, event);
    }

    espnow_send_cmd(g_node4_mac, CMD_STOP_STREAM, 0, 0);
    ESP_LOGI(TAG, "=== Pipeline complete ===");
}

static void run_pipeline(int location) {
    if (xSemaphoreTake(g_pipeline_mtx, pdMS_TO_TICKS(15000)) == pdTRUE) {
        face_pipeline(location);
        xSemaphoreGive(g_pipeline_mtx);
    } else {
        ESP_LOGW(TAG, "Pipeline mutex timeout - trigger at loc=%d skipped.", location);
    }
}

/* ================================================================
   ESP-NOW NODE HANDLERS
   ================================================================ */
static void handle_node2(const espnow_packet_t *pkt) {
    g_dash.last_n2_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (pkt->location == LOC_HEARTBEAT) {
        ESP_LOGD(TAG, "[NODE_2] Heartbeat.");
        return;
    }

    blynk_write_str(VPIN_NODE_NAME, (char *)pkt->name);

    if (pkt->location == LOC_INTRUDER) {
        blynk_write_str(VPIN_EVENT_LABEL, "Unknown RFID at Gate - INTRUDER!");
        blynk_write_int(VPIN_SEC_ALERT, 1);
        ESP_LOGW(TAG, "[NODE_2] Unknown RFID intruder.");
        g_dash.sec_alert = 1;
        dash_push_event(2, "Unknown RFID at Gate - INTRUDER!");
        run_pipeline(LOC_GATE);
        return;
    }

    char event[64];
    snprintf(event, sizeof(event), "%s at %s",
             user_name(pkt->userID), location_name(pkt->location));
    blynk_write_str(VPIN_EVENT_LABEL, event);
    blynk_write_int(VPIN_USER_ID,   pkt->userID);
    blynk_write_int(VPIN_LOCATION,  pkt->location);
    blynk_write_int(VPIN_SEC_ALERT, 0);
    ESP_LOGI(TAG, "[NODE_2] %s", event);
    g_dash.sec_alert = 0;
    dash_push_event(0, event);
    run_pipeline(pkt->location);
}

static void handle_node3(const espnow_packet_t *pkt) {
    blynk_write_float(VPIN_LPG,      pkt->lpg);
    blynk_write_float(VPIN_CH4,      pkt->CH4);
    blynk_write_float(VPIN_SMOKE,    pkt->smoke);
    blynk_write_float(VPIN_HYDROGEN, pkt->hydrogen);
    blynk_write_str(VPIN_NODE_NAME,  (char *)pkt->name);

    g_dash.last_n3_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    g_dash.lpg   = pkt->lpg;
    g_dash.ch4   = pkt->CH4;
    g_dash.smoke = pkt->smoke;
    g_dash.h2    = pkt->hydrogen;

    bool gas_now = (pkt->lpg      > THRESHOLD_LPG  ||
                    pkt->CH4      > THRESHOLD_CH4  ||
                    pkt->smoke    > THRESHOLD_SMOKE ||
                    pkt->hydrogen > THRESHOLD_H2);

    bool pir_now = (pkt->location == LOC_INTRUDER && !gas_now);

    if (gas_now != s_gas_was_danger) {
        s_gas_was_danger = gas_now;
        g_dash.gas_alert = gas_now ? 1 : 0;
        if (gas_now) {
            char msg[96];
            snprintf(msg, sizeof(msg),
                     "GAS ALERT - LPG:%.0f CH4:%.0f Smoke:%.0f H2:%.0f ppm",
                     (double)pkt->lpg, (double)pkt->CH4,
                     (double)pkt->smoke, (double)pkt->hydrogen);
            blynk_write_str(VPIN_EVENT_LABEL, msg);
            blynk_write_int(VPIN_GAS_ALERT, 1);
            ESP_LOGW(TAG, "[NODE_3] %s", msg);
            dash_push_event(2, msg);
            run_pipeline(LOC_INTRUDER);
        } else {
            blynk_write_str(VPIN_EVENT_LABEL, "Gas levels returned to normal.");
            blynk_write_int(VPIN_GAS_ALERT, 0);
            ESP_LOGI(TAG, "[NODE_3] Gas normal.");
            dash_push_event(0, "Gas levels returned to normal.");
        }
    }

    if (pir_now != s_pir_was_active) {
        s_pir_was_active = pir_now;
        g_dash.sec_alert = pir_now ? 1 : 0;
        g_dash.pir       = pir_now ? 1 : 0;
        if (pir_now) {
            blynk_write_str(VPIN_EVENT_LABEL, "PIR motion detected - INTRUDER!");
            blynk_write_int(VPIN_SEC_ALERT, 1);
            blynk_write_int(VPIN_PIR_STATUS, 1);
            ESP_LOGW(TAG, "[NODE_3] PIR intruder.");
            dash_push_event(2, "PIR motion detected - INTRUDER!");
            run_pipeline(LOC_INTRUDER);
        } else {
            blynk_write_int(VPIN_PIR_STATUS, 0);
            blynk_write_int(VPIN_SEC_ALERT, 0);
            ESP_LOGI(TAG, "[NODE_3] PIR clear.");
            dash_push_event(0, "PIR clear.");
        }
    }

    if (pkt->location == LOC_HEARTBEAT) {
        ESP_LOGD(TAG, "[NODE_3] HB: LPG=%.1f CH4=%.1f Smoke=%.1f H2=%.1f",
                 (double)pkt->lpg, (double)pkt->CH4,
                 (double)pkt->smoke, (double)pkt->hydrogen);
    }
}

static void handle_node4(const espnow_packet_t *pkt) {
    g_dash.last_n4_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
    blynk_write_str(VPIN_NODE_NAME, (char *)pkt->name);
    if (pkt->location == LOC_HEARTBEAT) {
        ESP_LOGD(TAG, "[NODE_4] Heartbeat.");
        return;
    }
    if (pkt->location == LOC_INTRUDER) {
        ESP_LOGI(TAG, "[NODE_4] Motion at door.");
        blynk_write_str(VPIN_EVENT_LABEL, "Motion at door - verifying...");
        g_dash.sec_alert = 1;
        dash_push_event(1, "Motion at door - verifying...");
        run_pipeline(LOC_MAIN_DOOR);
    }
}

static void process_packet(const espnow_packet_t *pkt) {
    ESP_LOGI(TAG, "PKT from=%.10s uid=%d loc=%d cmd=%d",
             pkt->name, pkt->userID, pkt->location, pkt->command);

    if      (strncmp(pkt->name, "NODE_2", 6) == 0) handle_node2(pkt);
    else if (strncmp(pkt->name, "NODE_3", 6) == 0) handle_node3(pkt);
    else if (strncmp(pkt->name, "NODE_4", 6) == 0) handle_node4(pkt);
    else    ESP_LOGW(TAG, "Unknown sender: %.10s", pkt->name);
}

/* ================================================================
   HTTP HANDLERS
   ================================================================ */
static esp_err_t status_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    if (xSemaphoreTake(g_dash_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
        httpd_resp_sendstr(req, "{\"error\":\"busy\"}");
        return ESP_OK;
    }

    uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

    int n2 = (g_dash.last_n2_ms && (now_ms - g_dash.last_n2_ms) < NODE_TIMEOUT_MS) ? 1 : 0;
    int n3 = (g_dash.last_n3_ms && (now_ms - g_dash.last_n3_ms) < NODE_TIMEOUT_MS) ? 1 : 0;
    int n4 = (g_dash.last_n4_ms && (now_ms - g_dash.last_n4_ms) < NODE_TIMEOUT_MS) ? 1 : 0;

    char ev_buf[640] = "[";
    int  count = g_dash.ev_count < DASH_EVENTS ? g_dash.ev_count : DASH_EVENTS;
    for (int i = 0; i < count; i++) {
        int idx = ((g_dash.ev_head - 1 - i) + DASH_EVENTS) % DASH_EVENTS;
        const dash_event_t *e = &g_dash.events[idx];

        char safe[96] = {0};
        int  si = 0;
        for (int k = 0; e->msg[k] && si < (int)sizeof(safe) - 2; k++) {
            if (e->msg[k] == '"') safe[si++] = '\\';
            safe[si++] = e->msg[k];
        }

        char tmp[180];
        snprintf(tmp, sizeof(tmp),
                 "%s{\"t\":\"%s\",\"lvl\":%d,\"msg\":\"%s\"}",
                 i ? "," : "", e->t, e->lvl, safe);
        strncat(ev_buf, tmp, sizeof(ev_buf) - strlen(ev_buf) - 2);
    }
    strcat(ev_buf, "]");

    uint32_t uptime_s = now_ms / 1000;

    float lpg = g_dash.lpg, ch4 = g_dash.ch4, smoke = g_dash.smoke, h2 = g_dash.h2;
    int   gas = g_dash.gas_alert, sec = g_dash.sec_alert, pir = g_dash.pir;
    char  face[80];
    strncpy(face, g_dash.face, sizeof(face) - 1);

    xSemaphoreGive(g_dash_mtx);

    char buf[1024];
    int  len = snprintf(buf, sizeof(buf),
        "{"
          "\"n2\":%d,\"n3\":%d,\"n4\":%d,"
          "\"lpg\":%.1f,\"ch4\":%.1f,\"smoke\":%.1f,\"h2\":%.1f,"
          "\"gas\":%d,\"sec\":%d,\"pir\":%d,"
          "\"face\":\"%s\","
          "\"up\":%lu,"
          "\"events\":%s"
        "}",
        n2, n3, n4, lpg, ch4, smoke, h2, gas, sec, pir, face,
        (unsigned long)uptime_s, ev_buf
    );

    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

static esp_err_t dashboard_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    size_t html_len = dashboard_html_end - dashboard_html_start;
    httpd_resp_send(req, (const char *)dashboard_html_start, (ssize_t)html_len);
    return ESP_OK;
}

static httpd_handle_t s_webhook = NULL;

static int parse_vpin(httpd_req_t *req) {
    char query[32] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return -1;
    char ps[8] = {0};
    if (httpd_query_key_value(query, "pin", ps, sizeof(ps)) != ESP_OK) return -1;
    return (ps[0] == 'V' || ps[0] == 'v') ? atoi(ps + 1) : -1;
}

static esp_err_t webhook_handler(httpd_req_t *req) {
    int pin = parse_vpin(req);
    ESP_LOGI(TAG, "[WEBHOOK] V%d received.", pin);
    httpd_resp_sendstr(req, "OK");
    if (pin >= 0) {
        if (xQueueSend(g_webhook_queue, &pin, pdMS_TO_TICKS(0)) != pdTRUE) {
            ESP_LOGW(TAG, "[WEBHOOK] Queue full - V%d dropped.", pin);
        }
    }
    return ESP_OK;
}

static void start_webhook_server(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port    = WEBHOOK_SERVER_PORT;
    cfg.ctrl_port      = 32769;
    cfg.max_uri_handlers = 8;

    if (httpd_start(&s_webhook, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Webhook server failed to start.");
        return;
    }

    httpd_uri_t uri_blynk = { .uri = "/blynk", .method = HTTP_GET, .handler = webhook_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_webhook, &uri_blynk);

    httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_webhook, &uri_status);

    httpd_uri_t uri_dash = { .uri = "/", .method = HTTP_GET, .handler = dashboard_handler, .user_ctx = NULL };
    httpd_register_uri_handler(s_webhook, &uri_dash);

    ESP_LOGI(TAG, "Webhook server on :%d/blynk", WEBHOOK_SERVER_PORT);
    ESP_LOGI(TAG, "Dashboard:        http://<NODE1_IP>:%d/", WEBHOOK_SERVER_PORT);
}

/* ================================================================
   TASKS
   ================================================================ */
static void webhook_task(void *pv) {
    int pin;
    while (1) {
        if (xQueueReceive(g_webhook_queue, &pin, portMAX_DELAY) != pdTRUE) continue;
        ESP_LOGI(TAG, "[WEBHOOK_TASK] Executing V%d action.", pin);
        switch (pin) {
            case VPIN_MANUAL_CAP:
                ESP_LOGI(TAG, "[WEBHOOK_TASK] Manual verify.");
                run_pipeline(LOC_MAIN_DOOR);
                break;
            case VPIN_DOOR_OVERRIDE:
                ESP_LOGI(TAG, "[WEBHOOK_TASK] Door override -> NODE_2.");
                espnow_send_cmd(g_node2_mac, CMD_IDLE, 0, LOC_MAIN_DOOR);
                break;
            case VPIN_VENT_OVERRIDE:
                ESP_LOGI(TAG, "[WEBHOOK_TASK] Vent ON -> NODE_3.");
                espnow_send_cmd(g_node3_mac, CMD_VENT_ON, 0, 0);
                break;
            case VPIN_VALVE_OVERRIDE:
                ESP_LOGI(TAG, "[WEBHOOK_TASK] Valve OPEN -> NODE_3.");
                espnow_send_cmd(g_node3_mac, CMD_VALVE_ON, 0, 0);
                break;
            default:
                ESP_LOGW(TAG, "[WEBHOOK_TASK] Unhandled pin V%d.", pin);
                break;
        }
    }
}

static void worker_task(void *pv) {
    espnow_packet_t pkt;
    while (1) {
        if (xQueueReceive(g_packet_queue, &pkt, portMAX_DELAY) == pdTRUE) {
            process_packet(&pkt);
        }
    }
}

/* ================================================================
   VOICE REC / GPIO IMPLEMENTATION
   ================================================================ */
static void init_gpio(void) {
  uint64_t output_pins =
      (1ULL << KITCHEN_LIGHT_PIN) | (1ULL << BEDROOM_LIGHT_PIN) |
      (1ULL << PARLOUR_LIGHT_PIN) | (1ULL << OUTSIDE_LIGHT_PIN) |
      (1ULL << COOLING_PIN) | (1ULL << LOCKDOWN_PIN) | (1ULL << BUZZER_PIN);

  gpio_config_t io_conf = {
      .pin_bit_mask = output_pins,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&io_conf);

  gpio_set_level(KITCHEN_LIGHT_PIN, 0);
  gpio_set_level(BEDROOM_LIGHT_PIN, 0);
  gpio_set_level(PARLOUR_LIGHT_PIN, 0);
  gpio_set_level(OUTSIDE_LIGHT_PIN, 0);
  gpio_set_level(COOLING_PIN, 0);
  gpio_set_level(LOCKDOWN_PIN, 0);
  gpio_set_level(BUZZER_PIN, 0);

  ESP_LOGI(TAG, "GPIO initialized.");
}

static void init_servo_channel(ledc_channel_t ch, gpio_num_t pin) {
  ledc_channel_config_t ch_cfg = {
      .gpio_num = pin,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = ch,
      .timer_sel = LEDC_TIMER_0,
      .duty = SERVO_DUTY_CLOSED,
      .hpoint = 0,
  };
  ledc_channel_config(&ch_cfg);
}

static void init_servos(void) {
  ledc_timer_config_t timer_cfg = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = SERVO_TIMER_RES,
      .timer_num = LEDC_TIMER_0,
      .freq_hz = SERVO_FREQ_HZ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  ledc_timer_config(&timer_cfg);

  init_servo_channel(BEDROOM_SERVO_1_CH, BEDROOM_SERVO_1_PIN);
  init_servo_channel(BEDROOM_SERVO_2_CH, BEDROOM_SERVO_2_PIN);
  init_servo_channel(PARLOUR_SERVO_1_CH, PARLOUR_SERVO_1_PIN);
  init_servo_channel(PARLOUR_SERVO_2_CH, PARLOUR_SERVO_2_PIN);

  ESP_LOGI(TAG, "Servos initialized.");
}

static void feedback_task(void *arg) {
  beep_type_t request;
  while (1) {
    if (xQueueReceive(s_feedback_queue, &request, portMAX_DELAY) == pdTRUE) {
      gpio_set_level(BUZZER_PIN, 1);
      vTaskDelay(pdMS_TO_TICKS(150));
      gpio_set_level(BUZZER_PIN, 0);
      if (request == BEEP_DOUBLE) {
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(BUZZER_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(150));
        gpio_set_level(BUZZER_PIN, 0);
      }
    }
  }
}

static void trigger_feedback(beep_type_t type) {
  if (s_feedback_queue)
    xQueueSend(s_feedback_queue, &type, 0);
}

static void set_servo_duty(ledc_channel_t ch, uint32_t duty) {
  ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

static void run_blind(ledc_channel_t ch1, ledc_channel_t ch2, bool open) {
  uint32_t duty = open ? SERVO_DUTY_OPEN : SERVO_DUTY_CLOSED;
  ESP_LOGI(TAG, "Servo -> %s", open ? "OPEN" : "CLOSED");
  set_servo_duty(ch1, duty);
  set_servo_duty(ch2, duty);
}

static void handle_command(int cmd_id) {
  trigger_feedback(BEEP_SINGLE);
  switch (cmd_id) {
  case 1:
    ESP_LOGW(TAG, "ACTION -> Kitchen Light ON");
    gpio_set_level(KITCHEN_LIGHT_PIN, 1);
    dash_push_event(0, "VOICE: Kitchen Light ON");
    break;
  case 2:
    ESP_LOGW(TAG, "ACTION -> Kitchen Light OFF");
    gpio_set_level(KITCHEN_LIGHT_PIN, 0);
    dash_push_event(0, "VOICE: Kitchen Light OFF");
    break;
  case 3:
    ESP_LOGW(TAG, "ACTION -> Bedroom Light ON");
    gpio_set_level(BEDROOM_LIGHT_PIN, 1);
    dash_push_event(0, "VOICE: Bedroom Light ON");
    break;
  case 4:
    ESP_LOGW(TAG, "ACTION -> Bedroom Light OFF");
    gpio_set_level(BEDROOM_LIGHT_PIN, 0);
    dash_push_event(0, "VOICE: Bedroom Light OFF");
    break;
  case 5:
    ESP_LOGW(TAG, "ACTION -> Parlour Light ON");
    gpio_set_level(PARLOUR_LIGHT_PIN, 1);
    dash_push_event(0, "VOICE: Parlour Light ON");
    break;
  case 6:
    ESP_LOGW(TAG, "ACTION -> Parlour Light OFF");
    gpio_set_level(PARLOUR_LIGHT_PIN, 0);
    dash_push_event(0, "VOICE: Parlour Light OFF");
    break;
  case 7:
    ESP_LOGW(TAG, "ACTION -> Bedroom Blinds OPEN");
    run_blind(BEDROOM_SERVO_1_CH, BEDROOM_SERVO_2_CH, true);
    bedroom_blinds_open = true;
    dash_push_event(0, "VOICE: Bedroom Blinds OPEN");
    break;
  case 8:
    ESP_LOGW(TAG, "ACTION -> Bedroom Blinds CLOSE");
    run_blind(BEDROOM_SERVO_1_CH, BEDROOM_SERVO_2_CH, false);
    bedroom_blinds_open = false;
    dash_push_event(0, "VOICE: Bedroom Blinds CLOSE");
    break;
  case 9:
    ESP_LOGW(TAG, "ACTION -> Parlour Blinds OPEN");
    run_blind(PARLOUR_SERVO_1_CH, PARLOUR_SERVO_2_CH, true);
    parlour_blinds_open = true;
    dash_push_event(0, "VOICE: Parlour Blinds OPEN");
    break;
  case 10:
    ESP_LOGW(TAG, "ACTION -> Parlour Blinds CLOSE");
    run_blind(PARLOUR_SERVO_1_CH, PARLOUR_SERVO_2_CH, false);
    parlour_blinds_open = false;
    dash_push_event(0, "VOICE: Parlour Blinds CLOSE");
    break;
  case 11:
    ESP_LOGW(TAG, "ACTION -> Cooling ON");
    gpio_set_level(COOLING_PIN, 1);
    dash_push_event(0, "VOICE: Cooling ON");
    break;
  case 12:
    ESP_LOGW(TAG, "ACTION -> Cooling OFF");
    gpio_set_level(COOLING_PIN, 0);
    dash_push_event(0, "VOICE: Cooling OFF");
    break;
  case 13:
    ESP_LOGW(TAG, "ACTION -> Outside Light ON");
    gpio_set_level(OUTSIDE_LIGHT_PIN, 1);
    dash_push_event(0, "VOICE: Outside Light ON");
    break;
  case 14:
    ESP_LOGW(TAG, "ACTION -> Outside Light OFF");
    gpio_set_level(OUTSIDE_LIGHT_PIN, 0);
    dash_push_event(0, "VOICE: Outside Light OFF");
    break;
  case 15:
    ESP_LOGE(TAG, "ACTION -> LOCKDOWN ACTIVATED");
    gpio_set_level(LOCKDOWN_PIN, 1);
    dash_push_event(2, "VOICE: LOCKDOWN ACTIVATED!");
    espnow_send_cmd(g_node2_mac, CMD_IDLE, 0, LOC_INTRUDER);
    break;
  default:
    ESP_LOGE(TAG, "Unknown command ID: %d", cmd_id);
    break;
  }
}

static void init_mic(void) {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, NULL, &rx_chan);

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
      .slot_cfg =
          {
              .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
              .slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT,
              .slot_mode = I2S_SLOT_MODE_MONO,
              .slot_mask = I2S_STD_SLOT_LEFT,
              .ws_width = 32,
              .ws_pol = false,
              .bit_shift = true,
              .left_align = true,
              .big_endian = false,
              .bit_order_lsb = false,
          },
      .gpio_cfg =
          {
              .bclk = I2S_BCLK_IO,
              .ws = I2S_WS_IO,
              .din = I2S_DIN_IO,
              .dout = I2S_GPIO_UNUSED,
              .mclk = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false,
                               .bclk_inv = false,
                               .ws_inv = false},
          },
  };
  i2s_channel_init_std_mode(rx_chan, &std_cfg);
  i2s_channel_enable(rx_chan);
  ESP_LOGI(TAG, "Microphone initialized.");
}

static void feed_task(void *arg) {
  int chunksize = g_afe_handle->get_feed_chunksize(g_afe_data);
  int32_t *i2s_buff32 = malloc(chunksize * sizeof(int32_t));
  int16_t *i2s_buff16 = malloc(chunksize * sizeof(int16_t));
  assert(i2s_buff32 && i2s_buff16);

  int32_t sample_sum = 0;
  int chunk_cnt = 0;

  while (1) {
    size_t bytes_read;
    i2s_channel_read(rx_chan, i2s_buff32, chunksize * sizeof(int32_t),
                     &bytes_read, portMAX_DELAY);

    for (int i = 0; i < chunksize; i++) {
      int32_t s = i2s_buff32[i] >> 14;
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      i2s_buff16[i] = (int16_t)s;
      sample_sum += abs((int)i2s_buff16[i]);
    }

    chunk_cnt++;
    if (chunk_cnt >= 50) {
      int32_t avg = sample_sum / (50 * chunksize);
      if (avg < 10)
        ESP_LOGE(TAG, "MIC SILENT  (avg=%4ld) - check wiring!", avg);
      else if (avg < 300)
        ESP_LOGI(TAG, "MIC quiet   (avg=%4ld)", avg);
      else
        ESP_LOGI(TAG, "MIC SPEECH  (avg=%4ld) ✓", avg);
      sample_sum = 0;
      chunk_cnt = 0;
    }

    g_afe_handle->feed(g_afe_data, i2s_buff16);
  }
}

static void detect_task(void *arg) {
  char *mn_name = esp_srmodel_filter((srmodel_list_t *)arg, ESP_MN_PREFIX, ESP_MN_ENGLISH);
  if (!mn_name) {
    ESP_LOGE(TAG, "No English MultiNet model found.");
    vTaskDelete(NULL);
    return;
  }
  ESP_LOGI(TAG, "MultiNet model: %s", mn_name);

  esp_mn_iface_t *multinet = esp_mn_handle_from_name(mn_name);
  model_iface_data_t *mn_data = multinet->create(mn_name, 6000);

  esp_mn_commands_alloc(multinet, mn_data);

  for (int i = 0; i < CMD_COUNT; i++) {
    esp_mn_commands_add(CMD_DEFS[i].command_id, CMD_DEFS[i].phrase);
  }

  esp_mn_commands_update();
  esp_mn_commands_print();

  bool listening = false;
  ESP_LOGI(TAG, "=== SENTINEL ACTIVE (MN7) === Say 'Hi ESP'");

  while (1) {
    afe_fetch_result_t *res = g_afe_handle->fetch(g_afe_data);
    if (!res || res->ret_value == ESP_FAIL) continue;

    if (res->wakeup_state == WAKENET_DETECTED) {
      ESP_LOGW(TAG, ">>> WAKE WORD DETECTED");
      trigger_feedback(BEEP_SINGLE);
      multinet->clean(mn_data);
      listening = true;
    }

    if (listening) {
      esp_mn_state_t mn_state = multinet->detect(mn_data, res->data);

      switch (mn_state) {
      case ESP_MN_STATE_DETECTING:
        break;
      case ESP_MN_STATE_DETECTED: {
        esp_mn_results_t *mn_res = multinet->get_results(mn_data);
        if (mn_res->prob[0] >= MIN_COMMAND_CONFIDENCE) {
          handle_command(mn_res->command_id[0]);
        } else {
          trigger_feedback(BEEP_DOUBLE);
        }
        listening = false;
        break;
      }
      case ESP_MN_STATE_TIMEOUT:
        listening = false;
        break;
      default:
        break;
      }
    }
  }
}

/* ================================================================
   APP_MAIN
   ================================================================ */
void app_main(void) {
    ESP_LOGI(TAG, "=== NODE_1 Master (ESP32-S3) booting ===");

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    init_gpio();
    init_servos();
    init_mic();

    g_pipeline_mtx = xSemaphoreCreateMutex();
    g_webhook_queue = xQueueCreate(WEBHOOK_QUEUE_DEPTH, sizeof(int));
    g_dash_mtx = xSemaphoreCreateMutex();
    memset(&g_dash, 0, sizeof(g_dash));

    s_feedback_queue = xQueueCreate(5, sizeof(beep_type_t));
    xTaskCreate(feedback_task, "feedback", 2048, NULL, 4, NULL);

    ESP_ERROR_CHECK(wifi_init_sta());
    ESP_ERROR_CHECK(espnow_init());
    blynk_init();
    start_webhook_server();

    if (xTaskCreatePinnedToCore(worker_task, "worker", WORKER_TASK_STACK, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create worker task.");
    }
    if (xTaskCreatePinnedToCore(webhook_task, "webhook", WORKER_TASK_STACK, NULL, 3, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create webhook task.");
    }

    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "SR model init failed.");
        return;
    }

    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_config) {
        ESP_LOGE(TAG, "AFE config init failed.");
        return;
    }

    afe_config->aec_init = false;
    afe_config->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    afe_config->wakenet_init = true;
    afe_config->wakenet_mode = DET_MODE_90;

    g_afe_handle = (esp_afe_sr_iface_t *)esp_afe_handle_from_config(afe_config);
    g_afe_data = g_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);

    if (xTaskCreatePinnedToCore(feed_task, "feed", 8192, NULL, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create feed_task (out of memory?)");
    }
    if (xTaskCreatePinnedToCore(detect_task, "detect", 16384, (void *)models, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create detect_task (out of memory?)");
    }

    ESP_LOGI(TAG, "=== NODE_1 ready - Voice + Mesh active ===");
}