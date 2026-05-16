/*
 * wifi.c  -  WiFi APSTA init for ESP-NOW + local dashboard (ESP-IDF 6.x)
 *
 * CHANGE vs original:
 *   Mode changed from WIFI_MODE_STA → WIFI_MODE_APSTA
 *   AP config added: SSID "SENTINEL-HUB", WPA2, same channel as ESP-NOW.
 *   Phone connects to "SENTINEL-HUB" → opens 192.168.4.1:8080 → live dashboard.
 *
 *   ESP-NOW continues to work because:
 *     - STA interface still exists (ESP-NOW uses STA internally)
 *     - AP and ESP-NOW share the same channel (ESPNOW_CHANNEL)
 *     - No AP connection is attempted on the STA side
 */
#include "wifi.h"
#include "config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "WIFI";

/* AP credentials — phone connects to this hotspot */
#define AP_SSID     "SENTINEL-HUB"
#define AP_PASSWORD "sentinel123"
#define AP_MAX_CONN  4

esp_err_t wifi_init_sta(void) {
    /* Initialise netif and default event loop */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Create both netif interfaces */
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();   /* AP gets 192.168.4.1 by default */

    /* Init WiFi driver */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* APSTA: STA for ESP-NOW, AP for local dashboard clients */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));

    /* RAM-only storage — no NVS required */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* Configure the AP */
    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = (uint8_t)strlen(AP_SSID),
            .password       = AP_PASSWORD,
            .max_connection = AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
            .channel        = ESPNOW_CHANNEL,  /* must match ESP-NOW channel */
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    /* Start WiFi — AP is now broadcasting, STA is idle (no AP association) */
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Give radio 50ms to power on before channel lock */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Lock STA interface to the ESP-NOW channel */
    esp_err_t ch_err;
    int ch_retries = 0;
    do {
        ch_err = esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
        if (ch_err != ESP_OK) {
            ESP_LOGW(TAG, "Channel set retry %d (err 0x%x)...", ++ch_retries, ch_err);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    } while (ch_err != ESP_OK && ch_retries < 10);

    if (ch_err != ESP_OK) {
        ESP_LOGE(TAG, "Could not lock channel %d — ESP-NOW may not work reliably", ESPNOW_CHANNEL);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "WiFi APSTA ready — SSID: \"%s\"  channel: %d", AP_SSID, ESPNOW_CHANNEL);
    ESP_LOGI(TAG, "Dashboard: connect phone to \"%s\" → http://192.168.4.1:8080", AP_SSID);
    return ESP_OK;
}
