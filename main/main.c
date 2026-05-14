#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"

/* ESP-SR API */
#include "esp_afe_sr_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_mn_models.h"
#include "esp_wn_models.h"
#include "esp_mn_speech_commands.h"

static const char *TAG = "SENTINEL";

// ---------------------------------------------------------------------------
// I2S Microphone Pins (INMP441)
// ---------------------------------------------------------------------------
#define I2S_WS_IO     GPIO_NUM_4
#define I2S_BCLK_IO   GPIO_NUM_5
#define I2S_DIN_IO    GPIO_NUM_6

// ---------------------------------------------------------------------------
// Relay Output Pins
// ---------------------------------------------------------------------------
#define KITCHEN_LIGHT_PIN   GPIO_NUM_7
#define BEDROOM_LIGHT_PIN   GPIO_NUM_8
#define PARLOUR_LIGHT_PIN   GPIO_NUM_9
#define OUTSIDE_LIGHT_PIN   GPIO_NUM_10
#define COOLING_PIN         GPIO_NUM_11

// ---------------------------------------------------------------------------
// Blinds Motor Pins (L298N)
// ---------------------------------------------------------------------------
#define BEDROOM_BLIND_OPEN    GPIO_NUM_12
#define BEDROOM_BLIND_CLOSE   GPIO_NUM_13
#define PARLOUR_BLIND_OPEN    GPIO_NUM_14
#define PARLOUR_BLIND_CLOSE   GPIO_NUM_15

// ---------------------------------------------------------------------------
// Security & Feedback
// ---------------------------------------------------------------------------
#define LOCKDOWN_PIN            GPIO_NUM_16
#define BUZZER_PIN              GPIO_NUM_17
#define BLIND_MOTOR_DURATION_MS 3000

// ---------------------------------------------------------------------------
// Confidence threshold.
// Start at 0.10f to confirm commands are being detected at all.
// Once you see correct phrases appearing in the log, raise to 0.40-0.60f.
// ---------------------------------------------------------------------------
#define MIN_COMMAND_CONFIDENCE 0.10f

// ---------------------------------------------------------------------------
// Voice Command Table
// MUST be UPPERCASE for MultiNet7 — MN7's text-to-phoneme engine requires it.
// Lowercase will either be silently rejected or produce wrong phoneme mappings.
// ---------------------------------------------------------------------------
#define CMD_COUNT 15

typedef struct {
    const char *phrase;
    int         command_id;
} cmd_def_t;

static const cmd_def_t CMD_DEFS[CMD_COUNT] = {
    { "KITCHEN LIGHT ON",     1  },
    { "KITCHEN LIGHT OFF",    2  },
    { "BEDROOM LIGHT ON",     3  },
    { "BEDROOM LIGHT OFF",    4  },
    { "PARLOUR LIGHT ON",     5  },
    { "PARLOUR LIGHT OFF",    6  },
    { "BEDROOM BLINDS OPEN",  7  },
    { "BEDROOM BLINDS CLOSE", 8  },
    { "PARLOUR BLINDS OPEN",  9  },
    { "PARLOUR BLINDS CLOSE", 10 },
    { "COOLING ON",           11 },
    { "COOLING OFF",          12 },
    { "OUTSIDE LIGHT ON",     13 },
    { "OUTSIDE LIGHT OFF",    14 },
    { "SENTINEL LOCKDOWN",    15 },
};

static i2s_chan_handle_t rx_chan;

// Shared AFE handles
static esp_afe_sr_iface_t *g_afe_handle = NULL;
static esp_afe_sr_data_t  *g_afe_data   = NULL;

// Async buzzer queue
typedef enum { BEEP_SINGLE, BEEP_DOUBLE } beep_type_t;
static QueueHandle_t s_feedback_queue = NULL;

// Blind motor task arguments
typedef struct {
    gpio_num_t open_pin;
    gpio_num_t close_pin;
    bool       open;
} blind_args_t;

// ---------------------------------------------------------------------------
// GPIO Initialization
// ---------------------------------------------------------------------------
static void init_gpio(void) {
    uint64_t output_pins =
        (1ULL << KITCHEN_LIGHT_PIN)   | (1ULL << BEDROOM_LIGHT_PIN)   |
        (1ULL << PARLOUR_LIGHT_PIN)   | (1ULL << OUTSIDE_LIGHT_PIN)   |
        (1ULL << COOLING_PIN)         | (1ULL << BEDROOM_BLIND_OPEN)  |
        (1ULL << BEDROOM_BLIND_CLOSE) | (1ULL << PARLOUR_BLIND_OPEN)  |
        (1ULL << PARLOUR_BLIND_CLOSE) | (1ULL << LOCKDOWN_PIN)        |
        (1ULL << BUZZER_PIN);

    gpio_config_t io_conf = {
        .pin_bit_mask = output_pins,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    gpio_set_level(KITCHEN_LIGHT_PIN,   0);
    gpio_set_level(BEDROOM_LIGHT_PIN,   0);
    gpio_set_level(PARLOUR_LIGHT_PIN,   0);
    gpio_set_level(OUTSIDE_LIGHT_PIN,   0);
    gpio_set_level(COOLING_PIN,         0);
    gpio_set_level(BEDROOM_BLIND_OPEN,  0);
    gpio_set_level(BEDROOM_BLIND_CLOSE, 0);
    gpio_set_level(PARLOUR_BLIND_OPEN,  0);
    gpio_set_level(PARLOUR_BLIND_CLOSE, 0);
    gpio_set_level(LOCKDOWN_PIN,        0);
    gpio_set_level(BUZZER_PIN,          0);

    ESP_LOGI(TAG, "GPIO initialized. All outputs OFF.");
}

// ---------------------------------------------------------------------------
// Async Buzzer Task
// ---------------------------------------------------------------------------
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
    if (s_feedback_queue) xQueueSend(s_feedback_queue, &type, 0);
}

// ---------------------------------------------------------------------------
// Async Blind Motor Task
// ---------------------------------------------------------------------------
static void blind_worker_task(void *arg) {
    blind_args_t *args = (blind_args_t *)arg;
    gpio_set_level(args->open_pin,  0);
    gpio_set_level(args->close_pin, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    if (args->open) {
        ESP_LOGI(TAG, "Motor OPEN running...");
        gpio_set_level(args->open_pin, 1);
    } else {
        ESP_LOGI(TAG, "Motor CLOSE running...");
        gpio_set_level(args->close_pin, 1);
    }
    vTaskDelay(pdMS_TO_TICKS(BLIND_MOTOR_DURATION_MS));
    gpio_set_level(args->open_pin,  0);
    gpio_set_level(args->close_pin, 0);
    ESP_LOGI(TAG, "Motor stopped.");
    free(args);
    vTaskDelete(NULL);
}

static void run_blind(gpio_num_t open_pin, gpio_num_t close_pin, bool open) {
    blind_args_t *args = malloc(sizeof(blind_args_t));
    if (args) {
        args->open_pin  = open_pin;
        args->close_pin = close_pin;
        args->open      = open;
        xTaskCreate(blind_worker_task, "blind_work", 2048, (void *)args, 3, NULL);
    }
}

// ---------------------------------------------------------------------------
// Command Dispatcher
// ---------------------------------------------------------------------------
static void handle_command(int cmd_id) {
    trigger_feedback(BEEP_SINGLE);
    switch (cmd_id) {
        case 1:  ESP_LOGW(TAG, "ACTION → Kitchen Light ON");      gpio_set_level(KITCHEN_LIGHT_PIN, 1); break;
        case 2:  ESP_LOGW(TAG, "ACTION → Kitchen Light OFF");     gpio_set_level(KITCHEN_LIGHT_PIN, 0); break;
        case 3:  ESP_LOGW(TAG, "ACTION → Bedroom Light ON");      gpio_set_level(BEDROOM_LIGHT_PIN, 1); break;
        case 4:  ESP_LOGW(TAG, "ACTION → Bedroom Light OFF");     gpio_set_level(BEDROOM_LIGHT_PIN, 0); break;
        case 5:  ESP_LOGW(TAG, "ACTION → Parlour Light ON");      gpio_set_level(PARLOUR_LIGHT_PIN, 1); break;
        case 6:  ESP_LOGW(TAG, "ACTION → Parlour Light OFF");     gpio_set_level(PARLOUR_LIGHT_PIN, 0); break;
        case 7:  ESP_LOGW(TAG, "ACTION → Bedroom Blinds OPEN");   run_blind(BEDROOM_BLIND_OPEN,  BEDROOM_BLIND_CLOSE, true);  break;
        case 8:  ESP_LOGW(TAG, "ACTION → Bedroom Blinds CLOSE");  run_blind(BEDROOM_BLIND_OPEN,  BEDROOM_BLIND_CLOSE, false); break;
        case 9:  ESP_LOGW(TAG, "ACTION → Parlour Blinds OPEN");   run_blind(PARLOUR_BLIND_OPEN,  PARLOUR_BLIND_CLOSE, true);  break;
        case 10: ESP_LOGW(TAG, "ACTION → Parlour Blinds CLOSE");  run_blind(PARLOUR_BLIND_OPEN,  PARLOUR_BLIND_CLOSE, false); break;
        case 11: ESP_LOGW(TAG, "ACTION → Cooling ON");            gpio_set_level(COOLING_PIN, 1);       break;
        case 12: ESP_LOGW(TAG, "ACTION → Cooling OFF");           gpio_set_level(COOLING_PIN, 0);       break;
        case 13: ESP_LOGW(TAG, "ACTION → Outside Light ON");      gpio_set_level(OUTSIDE_LIGHT_PIN, 1); break;
        case 14: ESP_LOGW(TAG, "ACTION → Outside Light OFF");     gpio_set_level(OUTSIDE_LIGHT_PIN, 0); break;
        case 15: ESP_LOGE(TAG, "ACTION → LOCKDOWN ACTIVATED");    gpio_set_level(LOCKDOWN_PIN, 1);      break;
        default: ESP_LOGE(TAG, "Unknown command ID: %d", cmd_id);                                       break;
    }
}

// ---------------------------------------------------------------------------
// Microphone Initialization
// ---------------------------------------------------------------------------
static void init_mic(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, NULL, &rx_chan);

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT,
            .slot_mode      = I2S_SLOT_MODE_MONO,
            .slot_mask      = I2S_STD_SLOT_LEFT,
            .ws_width       = 32,
            .ws_pol         = false,
            .bit_shift      = true,
            .left_align     = true,
            .big_endian     = false,
            .bit_order_lsb  = false,
        },
        .gpio_cfg = {
            .bclk = I2S_BCLK_IO,
            .ws   = I2S_WS_IO,
            .din  = I2S_DIN_IO,
            .dout = I2S_GPIO_UNUSED,
            .mclk = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    i2s_channel_init_std_mode(rx_chan, &std_cfg);
    i2s_channel_enable(rx_chan);
    ESP_LOGI(TAG, "Microphone initialized (32-bit mono LEFT 16kHz).");
}

// ---------------------------------------------------------------------------
// FEED TASK — Core 0
// Reads 32-bit I2S frames, converts to 16-bit, feeds AFE pipeline.
// >> 14 shift: INMP441 outputs data in bits [31:8] of the 32-bit frame.
//              >> 14 maps that to a clean signed 16-bit range.
//              >> 12 was WRONG — it over-amplifies by 4x, causing saturation
//              and giving MN7 clipped/distorted audio that kills recognition.
// ---------------------------------------------------------------------------
static void feed_task(void *arg) {
    int      chunksize  = g_afe_handle->get_feed_chunksize(g_afe_data);
    int32_t *i2s_buff32 = malloc(chunksize * sizeof(int32_t));
    int16_t *i2s_buff16 = malloc(chunksize * sizeof(int16_t));
    assert(i2s_buff32 && i2s_buff16);

    int32_t sample_sum = 0;
    int     chunk_cnt  = 0;
    size_t  bytes_read;

    while (1) {
        i2s_channel_read(rx_chan, i2s_buff32,
                         chunksize * sizeof(int32_t), &bytes_read, portMAX_DELAY);

        for (int i = 0; i < chunksize; i++) {
            int32_t s = i2s_buff32[i] >> 14;   // correct shift for INMP441
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            i2s_buff16[i] = (int16_t)s;
            sample_sum   += abs((int)i2s_buff16[i]);
        }

        // -------------------------------------------------------------------
        // MIC DIAGNOSTIC — printed every 50 chunks (~1.5 s)
        //
        // What the values mean:
        //   avg < 10   → SILENT — check INMP441 wiring (VDD, GND, L/R pin to GND)
        //   avg 10–299 → quiet room — normal when no one is speaking
        //   avg ≥ 300  → speech/noise detected
        //
        // During clear speech directly at the mic you should see 500–4000+.
        // If speech only reaches ~100–200, the mic is too far away or the
        // L/R select pin is floating (tie it to GND for left-channel output).
        // -------------------------------------------------------------------
        chunk_cnt++;
        if (chunk_cnt >= 50) {
            int32_t avg = sample_sum / (50 * chunksize);
            if (avg < 10)
                ESP_LOGE(TAG, "MIC SILENT  (avg=%4ld) — check wiring!", avg);
            else if (avg < 300)
                ESP_LOGI(TAG, "MIC quiet   (avg=%4ld)", avg);
            else
                ESP_LOGI(TAG, "MIC SPEECH  (avg=%4ld) ✓", avg);
            sample_sum = 0;
            chunk_cnt  = 0;
        }

        g_afe_handle->feed(g_afe_data, i2s_buff16);
    }
}

// ---------------------------------------------------------------------------
// DETECT TASK — Core 1
// ---------------------------------------------------------------------------
static void detect_task(void *arg) {
    char *mn_name = esp_srmodel_filter((srmodel_list_t *)arg, ESP_MN_PREFIX, ESP_MN_ENGLISH);
    if (!mn_name) {
        ESP_LOGE(TAG, "No English MultiNet model found.");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "MultiNet model: %s", mn_name);

    esp_mn_iface_t     *multinet = esp_mn_handle_from_name(mn_name);
    model_iface_data_t *mn_data  = multinet->create(mn_name, 6000);

    // -------------------------------------------------------------------
    // MN7 Command Registration
    // esp_mn_commands_alloc  — binds the global command table to this instance
    // esp_mn_commands_add    — registers each UPPERCASE phrase + ID
    // esp_mn_commands_update — commits to the model; returns any errors
    // esp_mn_commands_print  — dumps the final registered list to serial
    //                          so you can confirm all 15 are accepted
    // -------------------------------------------------------------------
    esp_mn_commands_alloc(multinet, mn_data);

    for (int i = 0; i < CMD_COUNT; i++) {
        esp_err_t ret = esp_mn_commands_add(CMD_DEFS[i].command_id,
                                            CMD_DEFS[i].phrase);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Command rejected at add: [%2d] '%s'",
                     CMD_DEFS[i].command_id, CMD_DEFS[i].phrase);
        }
    }

    esp_mn_error_t *mn_errors = esp_mn_commands_update();
    if (mn_errors && mn_errors->num > 0) {
        ESP_LOGW(TAG, "%d command(s) rejected at update:", mn_errors->num);
        for (int i = 0; i < mn_errors->num; i++) {
            ESP_LOGW(TAG, "  ✗ '%s'", mn_errors->phrases[i]->string);
        }
    } else {
        ESP_LOGI(TAG, "All commands accepted by MN7.");
    }

    // Prints the full accepted command list to serial on boot —
    // verify all 15 appear here before testing recognition
    esp_mn_commands_print();

    bool listening = false;

    ESP_LOGI(TAG, "=== SENTINEL ACTIVE (MN7) === Say 'Hi ESP'");

    while (1) {
        afe_fetch_result_t *res = g_afe_handle->fetch(g_afe_data);
        if (!res || res->ret_value == ESP_FAIL) continue;

        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGW(TAG, ">>> WAKE WORD — speak your command now");
            trigger_feedback(BEEP_SINGLE);
            multinet->clean(mn_data);   // flush stale MN7 probability state
            listening = true;
        }

        if (listening) {
            esp_mn_state_t mn_state = multinet->detect(mn_data, res->data);

            switch (mn_state) {
                case ESP_MN_STATE_DETECTING:
                    // No log here — UART latency here causes AFE ringbuffer overflow
                    break;

                case ESP_MN_STATE_DETECTED: {
                    esp_mn_results_t *mn_res = multinet->get_results(mn_data);
                    // Full detection log: phrase string, ID, and confidence score
                    ESP_LOGW(TAG, "┌─ DETECTION RESULT ──────────────────────");
                    ESP_LOGW(TAG, "│  Phrase    : %s",   mn_res->string);
                    ESP_LOGW(TAG, "│  Command ID: %d",   mn_res->command_id[0]);
                    ESP_LOGW(TAG, "│  Confidence: %.3f", mn_res->prob[0]);
                    ESP_LOGW(TAG, "│  Threshold : %.3f", (float)MIN_COMMAND_CONFIDENCE);
                    ESP_LOGW(TAG, "└─────────────────────────────────────────");

                    if (mn_res->prob[0] >= MIN_COMMAND_CONFIDENCE) {
                        handle_command(mn_res->command_id[0]);
                    } else {
                        ESP_LOGW(TAG, "✗ REJECTED — %.3f below threshold %.3f",
                                 mn_res->prob[0], (float)MIN_COMMAND_CONFIDENCE);
                        trigger_feedback(BEEP_DOUBLE);
                    }
                    listening = false;
                    break;
                }

                case ESP_MN_STATE_TIMEOUT:
                    ESP_LOGW(TAG, "✗ TIMEOUT — no command recognised.");
                    listening = false;
                    break;

                default:
                    break;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Entry Point
// ---------------------------------------------------------------------------
void app_main(void) {
    init_gpio();
    init_mic();

    s_feedback_queue = xQueueCreate(5, sizeof(beep_type_t));
    xTaskCreate(feedback_task, "feedback", 2048, NULL, 4, NULL);

    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) { ESP_LOGE(TAG, "SR model init failed."); return; }

    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_config) { ESP_LOGE(TAG, "AFE config init failed."); return; }

    afe_config->aec_init           = false;
    afe_config->wakenet_model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    afe_config->wakenet_init       = true;
    afe_config->wakenet_mode       = DET_MODE_90;

    ESP_LOGI(TAG, "WakeNet: %s", afe_config->wakenet_model_name
                                  ? afe_config->wakenet_model_name : "NULL");

    g_afe_handle = (esp_afe_sr_iface_t *)esp_afe_handle_from_config(afe_config);
    g_afe_data   = g_afe_handle->create_from_config(afe_config);
    afe_config_free(afe_config);

    xTaskCreatePinnedToCore(feed_task,   "feed",   4096 * 8,  NULL,          4, NULL, 0);
    xTaskCreatePinnedToCore(detect_task, "detect", 4096 * 24, (void*)models, 5, NULL, 1);
}