/*
 * main.c  -  NODE_1 Master (ESP32-S3)
 * Unified Project: Voice Recognition + ESP-NOW + WiFi + Blynk + Face++ +
 * Dashboard
 */
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"

/* ESP-SR API */
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_wn_models.h"

extern "C" {
#include "blynk.h"
#include "config.h"
#include "espnow.h"
#include "face_verify.h"
#include "http_client.h"
#include "shared_types.h"
#include "wifi.h"

/* Dashboard HTML */
extern const uint8_t dashboard_html_start[] asm("_binary_dashboard_html_start");
extern const uint8_t dashboard_html_end[] asm("_binary_dashboard_html_end");
}

/* Edge Impulse SDK */
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

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
#define BLUE_LED_PIN GPIO_NUM_18
#define GREEN_LED_PIN GPIO_NUM_19

#define MIN_COMMAND_CONFIDENCE 0.10f
#define CMD_COUNT 15

/* Edge Impulse Static Ring Buffer & State Definitions */
#define EI_SAMPLE_COUNT 16000
static int16_t ei_audio_ring_buf[EI_SAMPLE_COUNT] = {0};
static int ei_ring_buf_head = 0;
static SemaphoreHandle_t g_ei_audio_mutex = NULL;
static int16_t local_inference_buf[EI_SAMPLE_COUNT] = {0};
static volatile bool g_ei_wakeup_triggered = false;
static volatile bool g_ei_listening = false;

static int get_audio_data(size_t offset, size_t length, float *out_ptr) {
  for (size_t i = 0; i < length; i++) {
    out_ptr[i] = (float)local_inference_buf[offset + i];
  }
  return 0;
}

static void ei_task(void *pvParameters) {
  ESP_LOGI(TAG, "Edge Impulse continuous classifier task active.");

  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    ESP_LOGI(TAG, "[EI] Label[%d] = %s", i,
             ei_classifier_inferencing_categories[i]);
  }

  while (1) {
    vTaskDelay(pdMS_TO_TICKS(100)); // Yield briefly between classifications

    if (!g_ei_audio_mutex)
      continue;

    if (xSemaphoreTake(g_ei_audio_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      int head = ei_ring_buf_head;
      int first_part = EI_SAMPLE_COUNT - head;
      memcpy(local_inference_buf, &ei_audio_ring_buf[head],
             first_part * sizeof(int16_t));
      if (head > 0) {
        memcpy(&local_inference_buf[first_part], ei_audio_ring_buf,
               head * sizeof(int16_t));
      }
      xSemaphoreGive(g_ei_audio_mutex);
    } else {
      continue; // Skip classification if buffer is locked (ensures no real-time
                // drops)
    }

    signal_t signal;
    signal.total_length = EI_SAMPLE_COUNT;
    signal.get_data = &get_audio_data;

    ei_impulse_result_t result = {0};
    EI_IMPULSE_ERROR r = run_classifier(&signal, &result, false);
    if (r != EI_IMPULSE_OK) {
      ESP_LOGE(TAG, "run_classifier failed: %d", r);
      continue;
    }

    float sentinel_val = result.classification[1].value;
    float noise_val = result.classification[0].value;
    float unknown_val = result.classification[2].value;

    if (sentinel_val >= 0.05f) {
      ESP_LOGI(TAG,
               "[EI Classifier] sentinel: %.2f  noise: %.2f  unknown: %.2f",
               (double)sentinel_val, (double)noise_val, (double)unknown_val);
    }

    if (sentinel_val >= 0.25f && !g_ei_listening) {
      ESP_LOGW(TAG, ">>> EI WAKE WORD DETECTED (score: %.2f)!",
               (double)sentinel_val);
      g_ei_wakeup_triggered = true;
    }
  }
}

typedef struct {
  const char *phrase;
  int command_id;
} cmd_def_t;

static const cmd_def_t CMD_DEFS[CMD_COUNT] = {
    {"TURN ON KITCHEN LIGHT", 1},  {"TURN OFF KITCHEN LIGHT", 2},
    {"TURN ON BEDROOM LIGHT", 3},  {"TURN OFF BEDROOM LIGHT", 4},
    {"TURN ON PARLOUR LIGHT", 5},  {"TURN OFF PARLOUR LIGHT", 6},
    {"OPEN BEDROOM BLINDS", 7},    {"CLOSE BEDROOM BLINDS", 8},
    {"OPEN PARLOUR BLINDS", 9},    {"CLOSE PARLOUR BLINDS", 10},
    {"TURN ON COOLING", 11},       {"TURN OFF COOLING", 12},
    {"TURN ON OUTSIDE LIGHT", 13}, {"TURN OFF OUTSIDE LIGHT", 14},
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

/* Binary semaphore: given by handle_node4 when NODE_4's stream-live ack
 * arrives (command=CMD_START_STREAM, location=LOC_MAIN_DOOR). face_pipeline
 * waits on this instead of a blind vTaskDelay(2000), eliminating the race
 * where NODE_1 fetched /capture before NODE_4's HTTP server was ready.    */
static SemaphoreHandle_t g_node4_stream_sem = NULL;

#define WEBHOOK_QUEUE_DEPTH 8 /* enlarged: supports V20-V43 + flash slider */
static QueueHandle_t g_webhook_queue = NULL;

#define DASH_EVENTS 20 /* enlarged: keep last 20 events */
#define NODE_TIMEOUT_MS 15000

typedef struct {
  char t[8];
  int lvl;
  char msg[96];
} dash_event_t;

static struct {
  float lpg, ch4, smoke, h2;
  int gas_alert;
  int sec_alert;
  int pir;
  char face[80];
  char last_img_url[256]; /* ImgBB URL of last captured face photo */
  uint32_t last_n2_ms;
  uint32_t last_n3_ms;
  uint32_t last_n4_ms;
  dash_event_t events[DASH_EVENTS];
  int ev_count;
  int ev_head;
} g_dash;

static SemaphoreHandle_t g_dash_mtx = NULL;

/* ================================================================
   LOCAL SNAPSHOT BUFFER
   Stores the last captured JPEG so it can be served at
   /snapshot.jpg without needing an internet connection.
   64 KB is enough for a VGA JPEG from the ESP32-CAM.
   Uses PSRAM when available; falls back to DRAM.
   ================================================================ */
#define SNAP_MAX_BYTES 65536
static uint8_t *g_snap_buf = NULL;
static size_t g_snap_len = 0;
static SemaphoreHandle_t g_snap_mtx = NULL;

/* ================================================================
   FORWARD DECLARATIONS
   Required because webhook_task (defined before these functions)
   calls run_blind, set_servo_duty, and run_pipeline which are
   defined later in the file. C requires declaration before use.
   ================================================================ */
static void set_servo_duty(ledc_channel_t ch, uint32_t duty);
static void run_blind(ledc_channel_t ch1, ledc_channel_t ch2, bool open);
static void run_pipeline(int location);

/* ----------------------------------------------------------------
 * DASHBOARD EVENTS
 * ---------------------------------------------------------------- */
static void dash_push_event(int lvl, const char *msg) {
  if (!g_dash_mtx)
    return;
  if (xSemaphoreTake(g_dash_mtx, pdMS_TO_TICKS(50)) != pdTRUE)
    return;

  uint32_t s = (xTaskGetTickCount() * portTICK_PERIOD_MS) / 1000;
  uint32_t hh = (s / 3600) % 24;
  uint32_t mm = (s % 3600) / 60;

  dash_event_t *e = &g_dash.events[g_dash.ev_head];
  snprintf(e->t, sizeof(e->t), "%02lu:%02lu", (unsigned long)hh,
           (unsigned long)mm);
  e->lvl = lvl;
  snprintf(e->msg, sizeof(e->msg), "%s", msg ? msg : "");

  g_dash.ev_head = (g_dash.ev_head + 1) % DASH_EVENTS;
  if (g_dash.ev_count < DASH_EVENTS)
    g_dash.ev_count++;

  xSemaphoreGive(g_dash_mtx);
}

/* ================================================================
   FACE VERIFICATION PIPELINE
   ================================================================ */
static void face_pipeline(int trigger_location) {
  ESP_LOGI(TAG, "=== Pipeline start loc=%d (%s) ===", trigger_location,
           location_name(trigger_location));

  espnow_send_cmd(g_node4_mac, CMD_START_STREAM, 0, trigger_location);

  /* FIX: Wait for NODE_4's stream-live ack (command=1, location=2) instead
   * of a blind 2-second delay. NODE_4 sends this after camera deinit +
   * reinit + HTTP server start (~250 ms typical). The 5-second timeout
   * covers slow reinits and gives the 3 JPEG retry attempts below a
   * running server to connect to.
   * Note: this only works because run_pipeline() now spawns a dedicated
   * task, freeing worker_task to drain g_packet_queue and process the ack
   * via handle_node4 → xSemaphoreGive(g_node4_stream_sem).              */
  if (g_node4_stream_sem) {
    xSemaphoreTake(g_node4_stream_sem, pdMS_TO_TICKS(0)); /* flush stale give */
    if (xSemaphoreTake(g_node4_stream_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
      ESP_LOGW(TAG, "[PIPELINE] NODE_4 ack timeout — attempting fetch anyway.");
    }
  } else {
    vTaskDelay(pdMS_TO_TICKS(2000)); /* fallback: semaphore not initialised */
  }

  uint8_t *jpeg = NULL;
  size_t jlen = 0;
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

  /* ---- 1. Store JPEG locally ΓÇö always works, no internet needed ---- */
  if (g_snap_buf && xSemaphoreTake(g_snap_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (jlen <= SNAP_MAX_BYTES) {
      memcpy(g_snap_buf, jpeg, jlen);
      g_snap_len = jlen;
      ESP_LOGI(TAG, "Snapshot stored locally (%u bytes).", (unsigned)jlen);
    } else {
      ESP_LOGW(TAG, "JPEG too large for snapshot buffer (%u > %d).",
               (unsigned)jlen, SNAP_MAX_BYTES);
    }
    xSemaphoreGive(g_snap_mtx);
  }

  /* Point dashboard at local snapshot immediately ΓÇö works offline */
  if (xSemaphoreTake(g_dash_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
    snprintf(g_dash.last_img_url, sizeof(g_dash.last_img_url), "%s",
             "http://192.168.4.1:8080/snapshot.jpg");
    xSemaphoreGive(g_dash_mtx);
  }

  /* ---- 2. Try ImgBB upload (online mode) ---- */
  /* If internet is available the ImgBB URL replaces the local one  */
  /* so Blynk and remote viewers get a stable public link.          */
  char img_url[256] = {0};
  if (http_upload_imgbb(jpeg, jlen, img_url, sizeof(img_url)) == ESP_OK &&
      img_url[0] != '\0') {
    blynk_set_property(VPIN_CAM_IMAGE, "url", img_url);
    ESP_LOGI(TAG, "ImgBB upload OK ΓåÆ %s", img_url);
    /* Upgrade dashboard URL to public ImgBB link */
    if (xSemaphoreTake(g_dash_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
      snprintf(g_dash.last_img_url, sizeof(g_dash.last_img_url), "%s", img_url);
      xSemaphoreGive(g_dash_mtx);
    }
  } else {
    ESP_LOGI(TAG, "ImgBB unavailable ΓÇö dashboard using local /snapshot.jpg.");
  }

  char identity[32] = "Unknown";
  float confidence = 0.0f;
  esp_err_t verr =
      face_verify(jpeg, jlen, identity, sizeof(identity), &confidence);

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
  ESP_LOGI(TAG, "Identity: %s  uid=%d  conf=%.1f%%", identity, user_id,
           (double)confidence);

  if (xSemaphoreTake(g_dash_mtx, pdMS_TO_TICKS(50)) == pdTRUE) {
    snprintf(g_dash.face, sizeof(g_dash.face), "%s|%.1f%%|%s", identity,
             (double)confidence, location_name(trigger_location));
    g_dash.sec_alert = (user_id > 0) ? 0 : 1;
    xSemaphoreGive(g_dash_mtx);
  }

  blynk_write_str(VPIN_FACE_RESULT, identity);
  blynk_write_int(VPIN_USER_ID, user_id);
  blynk_write_int(VPIN_LOCATION, trigger_location);

  if (user_id > 0) {
    char event[80];
    snprintf(event, sizeof(event), "%s verified at %s", identity,
             location_name(trigger_location));
    blynk_write_str(VPIN_EVENT_LABEL, event);
    blynk_write_int(VPIN_SEC_ALERT, 0);
    ESP_LOGI(TAG, "[GRANTED] %s (%.1f%%)", identity, (double)confidence);
    espnow_send_cmd(g_node2_mac, CMD_IDLE, user_id, trigger_location);
    dash_push_event(0, event);
  } else {
    char event[80];
    snprintf(event, sizeof(event), "Unknown face at %s - DENIED (%.0f%%)",
             location_name(trigger_location), (double)confidence);
    blynk_write_str(VPIN_EVENT_LABEL, event);
    blynk_write_int(VPIN_SEC_ALERT, 1);
    ESP_LOGW(TAG, "[DENIED] Unknown (conf=%.1f%%)", (double)confidence);
    dash_push_event(2, event);
  }

  espnow_send_cmd(g_node4_mac, CMD_STOP_STREAM, 0, 0);
  ESP_LOGI(TAG, "=== Pipeline complete ===");
}

/* pipeline_task_fn / run_pipeline
 * ─────────────────────────────────────────────────────────────────────────
 * FIX: Previously run_pipeline() called face_pipeline() directly from
 * worker_task, blocking it for the full pipeline duration (2s wait +
 * HTTP fetch + face verify — up to ~10s). During that block, worker_task
 * could not drain g_packet_queue, so NODE_4's stream-live ack (command=1,
 * location=2) sat unprocessed. face_pipeline() had no choice but to use a
 * blind vTaskDelay(2000) and hope the camera was ready.
 *
 * Fix: run_pipeline() now spawns a one-shot FreeRTOS task. worker_task
 * returns immediately and can receive NODE_4's ack. face_pipeline() then
 * waits on g_node4_stream_sem (given by handle_node4 on ack) with a 5-
 * second timeout instead of a fixed 2-second delay.
 * g_pipeline_mtx still prevents concurrent pipelines.
 * ─────────────────────────────────────────────────────────────────────────
 */
typedef struct { int location; } pipeline_arg_t;

static void pipeline_task_fn(void *arg) {
  pipeline_arg_t *p = (pipeline_arg_t *)arg;
  int loc = p->location;
  free(p);
  if (xSemaphoreTake(g_pipeline_mtx, pdMS_TO_TICKS(15000)) == pdTRUE) {
    face_pipeline(loc);
    xSemaphoreGive(g_pipeline_mtx);
  } else {
    ESP_LOGW(TAG, "Pipeline mutex timeout — trigger at loc=%d skipped.", loc);
  }
  vTaskDelete(NULL);
}

static void run_pipeline(int location) {
  pipeline_arg_t *p = (pipeline_arg_t *)malloc(sizeof(pipeline_arg_t));
  if (!p) {
    ESP_LOGE(TAG, "run_pipeline: OOM for arg struct.");
    return;
  }
  p->location = location;
  if (xTaskCreatePinnedToCore(pipeline_task_fn, "pipeline", 8192,
                               p, 3, NULL, 1) != pdPASS) {
    ESP_LOGE(TAG, "run_pipeline: failed to create task — trigger dropped.");
    free(p);
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
  snprintf(event, sizeof(event), "%s at %s", user_name(pkt->userID),
           location_name(pkt->location));
  blynk_write_str(VPIN_EVENT_LABEL, event);
  blynk_write_int(VPIN_USER_ID, pkt->userID);
  blynk_write_int(VPIN_LOCATION, pkt->location);
  blynk_write_int(VPIN_SEC_ALERT, 0);
  ESP_LOGI(TAG, "[NODE_2] %s", event);
  g_dash.sec_alert = 0;
  dash_push_event(0, event);
  run_pipeline(pkt->location);
}

static void handle_node3(const espnow_packet_t *pkt) {
  blynk_write_float(VPIN_LPG, pkt->lpg);
  blynk_write_float(VPIN_CH4, pkt->CH4);
  blynk_write_float(VPIN_SMOKE, pkt->smoke);
  blynk_write_float(VPIN_HYDROGEN, pkt->hydrogen);
  blynk_write_str(VPIN_NODE_NAME, (char *)pkt->name);

  g_dash.last_n3_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
  g_dash.lpg = pkt->lpg;
  g_dash.ch4 = pkt->CH4;
  g_dash.smoke = pkt->smoke;
  g_dash.h2 = pkt->hydrogen;

  bool gas_now = (pkt->lpg > THRESHOLD_LPG || pkt->CH4 > THRESHOLD_CH4 ||
                  pkt->smoke > THRESHOLD_SMOKE || pkt->hydrogen > THRESHOLD_H2);

  bool pir_now = (pkt->location == LOC_INTRUDER && !gas_now);

  if (gas_now != s_gas_was_danger) {
    s_gas_was_danger = gas_now;
    g_dash.gas_alert = gas_now ? 1 : 0;
    if (gas_now) {
      char msg[96];
      snprintf(msg, sizeof(msg),
               "GAS ALERT - LPG:%.0f CH4:%.0f Smoke:%.0f H2:%.0f ppm",
               (double)pkt->lpg, (double)pkt->CH4, (double)pkt->smoke,
               (double)pkt->hydrogen);
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
    g_dash.pir = pir_now ? 1 : 0;
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
             (double)pkt->lpg, (double)pkt->CH4, (double)pkt->smoke,
             (double)pkt->hydrogen);
  }
}

static void handle_node4(const espnow_packet_t *pkt) {
  g_dash.last_n4_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
  blynk_write_str(VPIN_NODE_NAME, (char *)pkt->name);

  if (pkt->location == LOC_HEARTBEAT) {
    ESP_LOGD(TAG, "[NODE_4] Heartbeat.");
    return;
  }

  /* Stream-live ack: NODE_4 sends command=CMD_START_STREAM(1),
   * location=LOC_MAIN_DOOR(2) once its HTTP server is ready.
   * Signal face_pipeline() to proceed with the /capture fetch.
   * FIX: previously this packet fell through both if-checks silently,
   * so face_pipeline had to use a blind 2-second wait instead.          */
  if (pkt->command == CMD_START_STREAM && pkt->location == LOC_MAIN_DOOR) {
    ESP_LOGI(TAG, "[NODE_4] Stream live — signalling pipeline to fetch.");
    if (g_node4_stream_sem)
      xSemaphoreGive(g_node4_stream_sem);
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
  ESP_LOGI(TAG, "PKT from=%.10s uid=%d loc=%d cmd=%d", pkt->name, pkt->userID,
           pkt->location, pkt->command);

  if (strncmp(pkt->name, "NODE_2", 6) == 0)
    handle_node2(pkt);
  else if (strncmp(pkt->name, "NODE_3", 6) == 0)
    handle_node3(pkt);
  else if (strncmp(pkt->name, "NODE_4", 6) == 0)
    handle_node4(pkt);
  else
    ESP_LOGW(TAG, "Unknown sender: %.10s", pkt->name);
}

/* ================================================================
   HTTP HANDLERS
   ================================================================ */
static esp_err_t status_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  if (xSemaphoreTake(g_dash_mtx, pdMS_TO_TICKS(100)) != pdTRUE) {
    httpd_resp_sendstr(req, "{\"error\":\"busy\"}");
    return ESP_OK;
  }

  uint32_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

  int n2 = (g_dash.last_n2_ms && (now_ms - g_dash.last_n2_ms) < NODE_TIMEOUT_MS)
               ? 1
               : 0;
  int n3 = (g_dash.last_n3_ms && (now_ms - g_dash.last_n3_ms) < NODE_TIMEOUT_MS)
               ? 1
               : 0;
  int n4 = (g_dash.last_n4_ms && (now_ms - g_dash.last_n4_ms) < NODE_TIMEOUT_MS)
               ? 1
               : 0;

  /* Heap-allocate large buffers ΓÇö ev_buf(2048) + buf(3072) + other locals
   * totalled ~6 KB on the stack, overflowing the httpd task stack (4-8 KB
   * default). Overflow corrupts 'len' to -1 (HTTPD_RESP_USE_STRLEN), making
   * httpd_resp_send call strlen() on a trashed pointer ΓåÆ LoadProhibited.   */
  char *ev_buf = (char *)malloc(2048);
  if (!ev_buf) {
    xSemaphoreGive(g_dash_mtx);
    httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
    return ESP_OK;
  }
  ev_buf[0] = '[';
  ev_buf[1] = '\0';

  int count = g_dash.ev_count < DASH_EVENTS ? g_dash.ev_count : DASH_EVENTS;
  for (int i = 0; i < count; i++) {
    int idx = ((g_dash.ev_head - 1 - i) + DASH_EVENTS) % DASH_EVENTS;
    const dash_event_t *e = &g_dash.events[idx];

    char safe[112] = {0};
    int si = 0;
    for (int k = 0; e->msg[k] && si < (int)sizeof(safe) - 2; k++) {
      if (e->msg[k] == '"' || e->msg[k] == '\\')
        safe[si++] = '\\';
      safe[si++] = e->msg[k];
    }

    char tmp[220];
    snprintf(tmp, sizeof(tmp), "%s{\"t\":\"%s\",\"lvl\":%d,\"msg\":\"%s\"}",
             i ? "," : "", e->t, e->lvl, safe);
    strncat(ev_buf, tmp, 2048 - strlen(ev_buf) - 2);
  }
  strcat(ev_buf, "]");

  uint32_t uptime_s = now_ms / 1000;
  float lpg = g_dash.lpg, ch4 = g_dash.ch4, smoke = g_dash.smoke,
        h2 = g_dash.h2;
  int gas = g_dash.gas_alert, sec = g_dash.sec_alert, pir = g_dash.pir;
  char face[80];
  snprintf(face, sizeof(face), "%s", g_dash.face);
  char img_url[256];
  snprintf(img_url, sizeof(img_url), "%s", g_dash.last_img_url);

  xSemaphoreGive(g_dash_mtx);

  /* JSON-escape the image URL */
  char safe_url[300] = {0};
  int ui = 0;
  for (int k = 0; img_url[k] && ui < (int)sizeof(safe_url) - 2; k++) {
    if (img_url[k] == '"')
      safe_url[ui++] = '\\';
    safe_url[ui++] = img_url[k];
  }

  char *buf = (char *)malloc(3072);
  if (!buf) {
    free(ev_buf);
    httpd_resp_sendstr(req, "{\"error\":\"oom\"}");
    return ESP_OK;
  }
  int len = snprintf(buf, 3072,
                     "{"
                     "\"n2\":%d,\"n3\":%d,\"n4\":%d,"
                     "\"lpg\":%.1f,\"ch4\":%.1f,\"smoke\":%.1f,\"h2\":%.1f,"
                     "\"gas\":%d,\"sec\":%d,\"pir\":%d,"
                     "\"face\":\"%s\","
                     "\"img\":\"%s\","
                     "\"up\":%lu,"
                     "\"events\":%s"
                     "}",
                     n2, n3, n4, lpg, ch4, smoke, h2, gas, sec, pir, face,
                     safe_url, (unsigned long)uptime_s, ev_buf);

  httpd_resp_send(req, buf, len);
  free(buf);
  free(ev_buf);
  return ESP_OK;
}

static esp_err_t dashboard_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  size_t html_len = dashboard_html_end - dashboard_html_start;
  httpd_resp_send(req, (const char *)dashboard_html_start, (ssize_t)html_len);
  return ESP_OK;
}

/* /api/events.csv ΓÇö Download Log button */
static esp_err_t events_csv_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/csv; charset=utf-8");
  httpd_resp_set_hdr(req, "Content-Disposition",
                     "attachment; filename=\"sentinel_events.csv\"");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_sendstr_chunk(req, "time,level,message\r\n");

  if (xSemaphoreTake(g_dash_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
  }

  int count = g_dash.ev_count < DASH_EVENTS ? g_dash.ev_count : DASH_EVENTS;
  char row[200];
  for (int i = count - 1; i >= 0; i--) {
    int idx = ((g_dash.ev_head - 1 - i) + DASH_EVENTS) % DASH_EVENTS;
    const dash_event_t *e = &g_dash.events[idx];
    const char *lvl_str = (e->lvl == 2)   ? "danger"
                          : (e->lvl == 1) ? "warning"
                                          : "info";
    char qmsg[120] = "\"";
    int qi = 1;
    for (int k = 0; e->msg[k] && qi < (int)sizeof(qmsg) - 3; k++) {
      if (e->msg[k] == '"')
        qmsg[qi++] = '"';
      qmsg[qi++] = e->msg[k];
    }
    qmsg[qi++] = '"';
    qmsg[qi] = '\0';
    snprintf(row, sizeof(row), "%s,%s,%s\r\n", e->t, lvl_str, qmsg);
    httpd_resp_sendstr_chunk(req, row);
  }

  xSemaphoreGive(g_dash_mtx);
  httpd_resp_sendstr_chunk(req, NULL);
  return ESP_OK;
}

/* ================================================================
   SNAPSHOT HANDLER  ΓÇö  GET /snapshot.jpg
   Serves the last captured JPEG directly from RAM.
   Works fully offline; no internet required.
   ================================================================ */
static esp_err_t snapshot_handler(httpd_req_t *req) {
  if (!g_snap_buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Buffer not allocated");
    return ESP_OK;
  }
  if (xSemaphoreTake(g_snap_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Busy");
    return ESP_OK;
  }
  if (g_snap_len == 0) {
    xSemaphoreGive(g_snap_mtx);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No snapshot yet");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, (const char *)g_snap_buf, (ssize_t)g_snap_len);
  xSemaphoreGive(g_snap_mtx);
  ESP_LOGI(TAG, "Snapshot served (%u bytes).", (unsigned)g_snap_len);
  return ESP_OK;
}

/* ================================================================
   SNAPSHOT DOWNLOAD HANDLER  ΓÇö  GET /snapshot.download
   Same as /snapshot.jpg but forces a file-save dialog on the
   browser instead of displaying inline.
   ================================================================ */
static esp_err_t snapshot_download_handler(httpd_req_t *req) {
  if (!g_snap_buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Buffer not allocated");
    return ESP_OK;
  }
  if (xSemaphoreTake(g_snap_mtx, pdMS_TO_TICKS(200)) != pdTRUE) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Busy");
    return ESP_OK;
  }
  if (g_snap_len == 0) {
    xSemaphoreGive(g_snap_mtx);
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No snapshot yet");
    return ESP_OK;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition",
                     "attachment; filename=\"sentinel_snapshot.jpg\"");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, (const char *)g_snap_buf, (ssize_t)g_snap_len);
  xSemaphoreGive(g_snap_mtx);
  ESP_LOGI(TAG, "Snapshot downloaded (%u bytes).", (unsigned)g_snap_len);
  return ESP_OK;
}

static httpd_handle_t s_webhook = NULL;

static int parse_vpin(httpd_req_t *req) {
  char query[32] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK)
    return -1;
  char ps[8] = {0};
  if (httpd_query_key_value(query, "pin", ps, sizeof(ps)) != ESP_OK)
    return -1;
  return (ps[0] == 'V' || ps[0] == 'v') ? atoi(ps + 1) : -1;
}

static esp_err_t webhook_handler(httpd_req_t *req) {
  int pin = parse_vpin(req);
  ESP_LOGI(TAG, "[WEBHOOK] V%d received.", pin);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_sendstr(req, "OK");

  if (pin < 0)
    return ESP_OK;

  /* Parse optional &val= (used by flash brightness slider V28) */
  char query[48] = {0};
  char valstr[8] = {0};
  int val = 0;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK)
    if (httpd_query_key_value(query, "val", valstr, sizeof(valstr)) == ESP_OK)
      val = atoi(valstr);

  /* Pack pin (low 16 bits) + val (high 16 bits) into one int */
  int packed = (val << 16) | (pin & 0xFFFF);
  if (xQueueSend(g_webhook_queue, &packed, pdMS_TO_TICKS(0)) != pdTRUE)
    ESP_LOGW(TAG, "[WEBHOOK] Queue full - V%d dropped.", pin);

  return ESP_OK;
}

static void start_webhook_server(void) {
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = WEBHOOK_SERVER_PORT;
  cfg.ctrl_port = 32769;
  cfg.max_uri_handlers = 14;
  cfg.stack_size = 10240; /* raised from 4096: status_handler uses ~2KB stack
                             after heap fix */

  if (httpd_start(&s_webhook, &cfg) != ESP_OK) {
    ESP_LOGE(TAG, "Webhook server failed to start.");
    return;
  }

  httpd_uri_t uri_blynk = {.uri = "/blynk",
                           .method = HTTP_GET,
                           .handler = webhook_handler,
                           .user_ctx = NULL};
  httpd_register_uri_handler(s_webhook, &uri_blynk);

  httpd_uri_t uri_status = {.uri = "/api/status",
                            .method = HTTP_GET,
                            .handler = status_handler,
                            .user_ctx = NULL};
  httpd_register_uri_handler(s_webhook, &uri_status);

  httpd_uri_t uri_csv = {.uri = "/api/events.csv",
                         .method = HTTP_GET,
                         .handler = events_csv_handler,
                         .user_ctx = NULL};
  httpd_register_uri_handler(s_webhook, &uri_csv);

  httpd_uri_t uri_snap = {.uri = "/snapshot.jpg",
                          .method = HTTP_GET,
                          .handler = snapshot_handler,
                          .user_ctx = NULL};
  httpd_register_uri_handler(s_webhook, &uri_snap);

  httpd_uri_t uri_snap_dl = {.uri = "/snapshot.download",
                             .method = HTTP_GET,
                             .handler = snapshot_download_handler,
                             .user_ctx = NULL};
  httpd_register_uri_handler(s_webhook, &uri_snap_dl);

  httpd_uri_t uri_dash = {.uri = "/",
                          .method = HTTP_GET,
                          .handler = dashboard_handler,
                          .user_ctx = NULL};
  httpd_register_uri_handler(s_webhook, &uri_dash);

  ESP_LOGI(TAG, "Webhook server on :%d/blynk", WEBHOOK_SERVER_PORT);
  ESP_LOGI(TAG, "Dashboard:     http://<NODE1_IP>:%d/", WEBHOOK_SERVER_PORT);
  ESP_LOGI(TAG, "Event CSV:     http://<NODE1_IP>:%d/api/events.csv",
           WEBHOOK_SERVER_PORT);
}

/* ================================================================
   TASKS
   ================================================================ */
static void webhook_task(void *pv) {
  int packed;
  while (1) {
    if (xQueueReceive(g_webhook_queue, &packed, portMAX_DELAY) != pdTRUE)
      continue;

    int pin = packed & 0xFFFF;         /* low 16 bits  = pin number  */
    int val = (packed >> 16) & 0xFFFF; /* high 16 bits = value 0-255 */
    ESP_LOGI(TAG, "[WEBHOOK_TASK] V%d val=%d", pin, val);

    switch (pin) {

    /* ── System ── */
    case VPIN_MANUAL_CAP:
      run_pipeline(LOC_MAIN_DOOR);
      break;

    case VPIN_DOOR_OVERRIDE: /* V13 — open main door */
      espnow_send_cmd(g_node2_mac, CMD_DOOR_OPEN, 0, LOC_MAIN_DOOR);
      dash_push_event(0, "REMOTE: Main Door OPEN");
      break;

    case VPIN_VENT_OVERRIDE: /* V14 — vent ON */
      espnow_send_cmd(g_node3_mac, CMD_VENT_ON, 0, 0);
      dash_push_event(0, "REMOTE: Vent ON");
      break;

    case VPIN_VALVE_OVERRIDE: /* V15 — valve OPEN */
      espnow_send_cmd(g_node3_mac, CMD_VALVE_ON, 0, 0);
      dash_push_event(1, "REMOTE: Gas Valve OPEN");
      break;

    /* ── Door controls ── */
    case 20:
      espnow_send_cmd(g_node2_mac, CMD_DOOR_OPEN, 0, LOC_MAIN_DOOR);
      dash_push_event(0, "REMOTE: Main Door OPEN");
      break;
    case 21:
      espnow_send_cmd(g_node2_mac, CMD_DOOR_CLOSE, 0, LOC_MAIN_DOOR);
      dash_push_event(0, "REMOTE: Main Door CLOSED");
      break;
    case 22:
      espnow_send_cmd(g_node3_mac, CMD_DOOR_OPEN, 0, LOC_BACK_DOOR);
      dash_push_event(0, "REMOTE: Back Door OPEN");
      break;
    case 23:
      espnow_send_cmd(g_node3_mac, CMD_DOOR_CLOSE, 0, LOC_BACK_DOOR);
      dash_push_event(0, "REMOTE: Back Door CLOSED");
      break;

    /* ── Valve controls ── */
    case 24:
      espnow_send_cmd(g_node3_mac, CMD_VALVE_ON, 0, 0);
      dash_push_event(1, "REMOTE: Gas Valve OPEN");
      break;
    case 25:
      espnow_send_cmd(g_node3_mac, CMD_VALVE_OFF, 0, 0);
      dash_push_event(0, "REMOTE: Gas Valve CLOSED");
      break;

    /* ── Vent controls ── */
    case 26:
      espnow_send_cmd(g_node3_mac, CMD_VENT_ON, 0, 0);
      dash_push_event(0, "REMOTE: Vent ON");
      break;
    case 27:
      espnow_send_cmd(g_node3_mac, CMD_VENT_OFF, 0, 0);
      dash_push_event(0, "REMOTE: Vent OFF");
      break;

    /* ── Flash brightness → NODE_4 LED ── */
    case 28: {
      int bri = val;
      if (bri < 0)
        bri = 0;
      if (bri > 255)
        bri = 255;
      espnow_send_cmd(g_node4_mac, CMD_SET_FLASH, bri, 0);
      ESP_LOGI(TAG, "[FLASH] NODE_4 brightness -> %d", bri);
      break;
    }

    /* ── Appliance overrides — NODE_1 GPIO direct ── */
    case 30:
      gpio_set_level(KITCHEN_LIGHT_PIN, 1);
      dash_push_event(0, "OVERRIDE: Kitchen Light ON");
      break;
    case 31:
      gpio_set_level(KITCHEN_LIGHT_PIN, 0);
      dash_push_event(0, "OVERRIDE: Kitchen Light OFF");
      break;
    case 32:
      gpio_set_level(BEDROOM_LIGHT_PIN, 1);
      dash_push_event(0, "OVERRIDE: Bedroom Light ON");
      break;
    case 33:
      gpio_set_level(BEDROOM_LIGHT_PIN, 0);
      dash_push_event(0, "OVERRIDE: Bedroom Light OFF");
      break;
    case 34:
      gpio_set_level(PARLOUR_LIGHT_PIN, 1);
      dash_push_event(0, "OVERRIDE: Parlour Light ON");
      break;
    case 35:
      gpio_set_level(PARLOUR_LIGHT_PIN, 0);
      dash_push_event(0, "OVERRIDE: Parlour Light OFF");
      break;
    case 36:
      gpio_set_level(OUTSIDE_LIGHT_PIN, 1);
      dash_push_event(0, "OVERRIDE: Outside Light ON");
      break;
    case 37:
      gpio_set_level(OUTSIDE_LIGHT_PIN, 0);
      dash_push_event(0, "OVERRIDE: Outside Light OFF");
      break;
    case 38:
      gpio_set_level(COOLING_PIN, 1);
      dash_push_event(0, "OVERRIDE: Cooling ON");
      break;
    case 39:
      gpio_set_level(COOLING_PIN, 0);
      dash_push_event(0, "OVERRIDE: Cooling OFF");
      break;
    case 40:
      run_blind(BEDROOM_SERVO_1_CH, BEDROOM_SERVO_2_CH, true);
      bedroom_blinds_open = true;
      dash_push_event(0, "OVERRIDE: Bedroom Blinds OPEN");
      break;
    case 41:
      run_blind(BEDROOM_SERVO_1_CH, BEDROOM_SERVO_2_CH, false);
      bedroom_blinds_open = false;
      dash_push_event(0, "OVERRIDE: Bedroom Blinds CLOSE");
      break;
    case 42:
      run_blind(PARLOUR_SERVO_1_CH, PARLOUR_SERVO_2_CH, true);
      parlour_blinds_open = true;
      dash_push_event(0, "OVERRIDE: Parlour Blinds OPEN");
      break;
    case 43:
      run_blind(PARLOUR_SERVO_1_CH, PARLOUR_SERVO_2_CH, false);
      parlour_blinds_open = false;
      dash_push_event(0, "OVERRIDE: Parlour Blinds CLOSE");
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
      (1ULL << COOLING_PIN) | (1ULL << LOCKDOWN_PIN) | (1ULL << BUZZER_PIN) |
      (1ULL << BLUE_LED_PIN) | (1ULL << GREEN_LED_PIN);

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
  gpio_set_level(BLUE_LED_PIN, 0);
  gpio_set_level(GREEN_LED_PIN, 0);

  ESP_LOGI(TAG, "GPIO initialized.");
}

static void init_servo_channel(ledc_channel_t ch, gpio_num_t pin) {
  ledc_channel_config_t ch_cfg = {};
  ch_cfg.gpio_num = pin;
  ch_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
  ch_cfg.channel = ch;
  ch_cfg.timer_sel = LEDC_TIMER_0;
  ch_cfg.duty = SERVO_DUTY_CLOSED;
  ch_cfg.hpoint = 0;
  ledc_channel_config(&ch_cfg);
}

static void init_servos(void) {
  ledc_timer_config_t timer_cfg = {};
  timer_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
  timer_cfg.duty_resolution = SERVO_TIMER_RES;
  timer_cfg.timer_num = LEDC_TIMER_0;
  timer_cfg.freq_hz = SERVO_FREQ_HZ;
  timer_cfg.clk_cfg = LEDC_AUTO_CLK;
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

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000);

  std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
  std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
  std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  std_cfg.slot_cfg.ws_width = 32;
  std_cfg.slot_cfg.ws_pol = false;
  std_cfg.slot_cfg.bit_shift = true;
  std_cfg.slot_cfg.left_align = true;
  std_cfg.slot_cfg.big_endian = false;
  std_cfg.slot_cfg.bit_order_lsb = false;

  std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.bclk = I2S_BCLK_IO;
  std_cfg.gpio_cfg.ws = I2S_WS_IO;
  std_cfg.gpio_cfg.din = I2S_DIN_IO;
  std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.ws_inv = false;
  i2s_channel_init_std_mode(rx_chan, &std_cfg);
  i2s_channel_enable(rx_chan);
  ESP_LOGI(TAG, "Microphone initialized.");
}

static void feed_task(void *arg) {
  int chunksize = g_afe_handle->get_feed_chunksize(g_afe_data);
  int32_t *i2s_buff32 = (int32_t *)malloc(chunksize * sizeof(int32_t));
  int16_t *i2s_buff16 = (int16_t *)malloc(chunksize * sizeof(int16_t));
  assert(i2s_buff32 && i2s_buff16);

  int32_t sample_sum = 0;
  int chunk_cnt = 0;

  while (1) {
    size_t bytes_read;
    i2s_channel_read(rx_chan, i2s_buff32, chunksize * sizeof(int32_t),
                     &bytes_read, portMAX_DELAY);

    for (int i = 0; i < chunksize; i++) {
      int32_t s = i2s_buff32[i] >> 14;
      if (s > 32767)
        s = 32767;
      if (s < -32768)
        s = -32768;
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

    /* Duplicate stream to Edge Impulse Circular Buffer */
    if (g_ei_audio_mutex && xSemaphoreTake(g_ei_audio_mutex, 0) == pdTRUE) {
      for (int i = 0; i < chunksize; i++) {
        ei_audio_ring_buf[ei_ring_buf_head] = i2s_buff16[i];
        ei_ring_buf_head = (ei_ring_buf_head + 1) % EI_SAMPLE_COUNT;
      }
      xSemaphoreGive(g_ei_audio_mutex);
    }

    g_afe_handle->feed(g_afe_data, i2s_buff16);
  }
}

static void detect_task(void *arg) {
  char *mn_name =
      esp_srmodel_filter((srmodel_list_t *)arg, ESP_MN_PREFIX, ESP_MN_ENGLISH);
  if (!mn_name) {
    ESP_LOGW(TAG, "srmodel_filter failed, using hardcoded mn7_en");
    mn_name = (char *)"mn7_en";
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

  /* Drain any backlogged AFE frames accumulated during command registration */
  ESP_LOGI(TAG, "Draining AFE buffer...");
  for (int i = 0; i < 200; i++) {
    afe_fetch_result_t *drain = g_afe_handle->fetch(g_afe_data);
    if (!drain)
      break;
  }
  ESP_LOGI(TAG, "AFE buffer drained.");

  g_ei_listening = false;
  ESP_LOGI(TAG, "=== SENTINEL ACTIVE (MN7) === Say your wake word");

  while (1) {
    afe_fetch_result_t *res = g_afe_handle->fetch(g_afe_data);
    if (!res || res->ret_value == ESP_FAIL)
      continue;

    /* Custom Wake Trigger from Edge Impulse Classifier */
    if (g_ei_wakeup_triggered) {
      g_ei_wakeup_triggered = false;
      gpio_set_level(BLUE_LED_PIN, 1);
      multinet->clean(mn_data);
      g_ei_listening = true;
      ESP_LOGW(TAG, ">>> LISTENING MODE ON — say a command now!");
    }

    if (g_ei_listening) {
      esp_mn_state_t mn_state = multinet->detect(mn_data, res->data);

      switch (mn_state) {
      case ESP_MN_STATE_DETECTING:
        break;
      case ESP_MN_STATE_DETECTED: {
        esp_mn_results_t *mn_res = multinet->get_results(mn_data);
        gpio_set_level(BLUE_LED_PIN, 0);
        if (mn_res->prob[0] >= MIN_COMMAND_CONFIDENCE) {
          gpio_set_level(GREEN_LED_PIN, 1);
          handle_command(mn_res->command_id[0]);
          vTaskDelay(pdMS_TO_TICKS(1000)); // Keep green LED on for 1s
          gpio_set_level(GREEN_LED_PIN, 0);
        } else {
          trigger_feedback(BEEP_DOUBLE);
        }
        g_ei_listening = false;
        break;
      }
      case ESP_MN_STATE_TIMEOUT:
        gpio_set_level(BLUE_LED_PIN, 0);
        g_ei_listening = false;
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
extern "C" void app_main(void) {
  ESP_LOGI(TAG, "=== NODE_1 Master (ESP32-S3) booting ===");

  esp_err_t nvs_err = nvs_flash_init();
  if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs_err);

  init_gpio();
  init_servos();
  init_mic();

  g_pipeline_mtx = xSemaphoreCreateMutex();
  g_node4_stream_sem = xSemaphoreCreateBinary();  /* for NODE_4 stream-live ack */
  g_webhook_queue = xQueueCreate(WEBHOOK_QUEUE_DEPTH, sizeof(int));
  g_dash_mtx = xSemaphoreCreateMutex();
  memset(&g_dash, 0, sizeof(g_dash));

  /* Allocate local snapshot buffer — prefer PSRAM, fall back to DRAM */
  g_snap_mtx = xSemaphoreCreateMutex();
  g_snap_buf = (uint8_t *)heap_caps_malloc(SNAP_MAX_BYTES,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!g_snap_buf) {
    ESP_LOGW(TAG, "PSRAM unavailable — allocating snapshot buffer in DRAM.");
    g_snap_buf = (uint8_t *)malloc(SNAP_MAX_BYTES);
  }
  if (g_snap_buf) {
    ESP_LOGI(TAG, "Snapshot buffer ready (%d KB).", SNAP_MAX_BYTES / 1024);
  } else {
    ESP_LOGE(TAG,
             "No RAM for snapshot buffer — /snapshot.jpg will be disabled.");
  }

  s_feedback_queue = xQueueCreate(5, sizeof(beep_type_t));
  xTaskCreate(feedback_task, "feedback", 2048, NULL, 4, NULL);

  ESP_ERROR_CHECK(wifi_init_sta());
  ESP_ERROR_CHECK(espnow_init());
  blynk_init();
  start_webhook_server();

  if (xTaskCreatePinnedToCore(worker_task, "worker", WORKER_TASK_STACK, NULL, 3,
                              NULL, 1) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create worker task.");
  }
  if (xTaskCreatePinnedToCore(webhook_task, "webhook", WORKER_TASK_STACK, NULL,
                              3, NULL, 1) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create webhook task.");
  }

  srmodel_list_t *models = esp_srmodel_init("model");
  if (!models) {
    ESP_LOGE(TAG, "SR model init failed.");
    return;
  }

  /* Initialize Edge Impulse audio mutex */
  g_ei_audio_mutex = xSemaphoreCreateMutex();

  afe_config_t *afe_config =
      afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
  if (!afe_config) {
    ESP_LOGE(TAG, "AFE config init failed.");
    return;
  }

  /* Disable Espressif WakeNet to free massive SRAM memory */
  afe_config->aec_init = false;
  afe_config->wakenet_model_name = NULL;
  afe_config->wakenet_init = false;
  afe_config->wakenet_mode = DET_MODE_90;

  g_afe_handle = (esp_afe_sr_iface_t *)esp_afe_handle_from_config(afe_config);
  g_afe_data = g_afe_handle->create_from_config(afe_config);
  afe_config_free(afe_config);

  if (xTaskCreatePinnedToCore(feed_task, "feed", 8192, NULL, 4, NULL, 0) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create feed_task (out of memory?)");
  }
  if (xTaskCreatePinnedToCore(detect_task, "detect", 16384, (void *)models, 5,
                              NULL, 1) != pdPASS) {
    ESP_LOGE(TAG, "Failed to create detect_task (out of memory?)");
  }
  if (xTaskCreatePinnedToCore(ei_task, "ei_infer", 8192, NULL, 3, NULL, 0) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to create ei_task (out of memory?)");
  }

  ESP_LOGI(TAG, "=== NODE_1 ready - Voice + Mesh active ===");
}