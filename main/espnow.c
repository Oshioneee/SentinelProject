/*
 * espnow.c  -  ESP-NOW implementation (ESP-IDF 6.x)
 *
 * Fixes applied for ESP-IDF v6.0 compatibility:
 *
 * FIX-A  Added #include "esp_mac.h"
 *        In IDF v6.0, MACSTR and MAC2STR moved out of esp_wifi.h into
 *        esp_mac.h. Without this include the macros are undefined,
 *        causing "expected ')' before 'MACSTR'" errors everywhere they
 *        appear inside ESP_LOGI / ESP_LOGE.
 *
 * FIX-B  Added #include "esp_wifi.h"
 *        ESP_IF_WIFI_STA was removed in IDF v6.0. The replacement is
 *        WIFI_IF_STA, defined in esp_wifi_types.h which is pulled in
 *        by esp_wifi.h.
 *
 * FIX-C  Changed p.ifidx = ESP_IF_WIFI_STA  →  p.ifidx = WIFI_IF_STA
 *        Direct rename required by IDF v6.0 breaking change.
 *
 * FIX-D  espnow.h now includes "freertos/FreeRTOS.h" before
 *        "freertos/queue.h". Without this, QueueHandle_t is undefined
 *        when the header is parsed on its own, causing the compiler to
 *        treat every subsequent declaration as a function parameter and
 *        producing hundreds of cascading "storage class specified for
 *        parameter" / "expected declaration specifiers" errors across
 *        esp_netif.h, esp_wifi.h, esp_log.h, and this file.
 *
 * The receive callback runs in the WiFi task and MUST be fast.
 * It copies 42 bytes into a FreeRTOS queue and returns.
 * The worker task in main.c does all actual processing.
 */
#include "espnow.h"
#include "config.h"
#include "esp_now.h"
#include "esp_wifi.h"       /* FIX-B: brings in WIFI_IF_STA              */
#include "esp_mac.h"        /* FIX-A: brings in MACSTR / MAC2STR         */
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "ESPNOW";

/* Exported queue handle - worker task reads from this */
QueueHandle_t g_packet_queue = NULL;

/* Real MAC addresses (confirmed from serial output) */
uint8_t g_node2_mac[6] = { 0xF0, 0x24, 0xF9, 0x0B, 0xAA, 0xD8 }; /* NODE_2 Security    */
uint8_t g_node3_mac[6] = { 0xF0, 0x24, 0xF9, 0x0C, 0x8D, 0x30 }; /* NODE_3 Environment */
uint8_t g_node4_mac[6] = { 0x68, 0x25, 0xDD, 0x2D, 0xD7, 0x1C }; /* NODE_4 Vision/CAM  */

/*
 * espnow_recv_cb()  -  called from the WiFi task.
 * IRAM_ATTR keeps this in fast RAM to avoid cache misses on rapid packets.
 * Non-blocking queue send: if queue is full, packet is dropped.
 * Dropping is preferable to stalling the WiFi task (watchdog would fire).
 */
static void IRAM_ATTR espnow_recv_cb(const esp_now_recv_info_t *info,
                                      const uint8_t             *data,
                                      int                        len) {
    if (!info || !data || len < (int)sizeof(espnow_packet_t)) return;

    espnow_packet_t pkt;
    memcpy(&pkt, data, sizeof(espnow_packet_t));

    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(g_packet_queue, &pkt, &woken);
    portYIELD_FROM_ISR(woken);
}

/* Add a peer to the ESP-NOW send list */
static void add_peer(const uint8_t *mac, const char *label) {
    esp_now_peer_info_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.peer_addr, mac, ESP_NOW_ETH_ALEN);
    p.channel = ESPNOW_CHANNEL;
    p.ifidx   = WIFI_IF_STA;   /* FIX-C: was ESP_IF_WIFI_STA (removed in IDF v6.0) */
    p.encrypt = false;
    esp_err_t e = esp_now_add_peer(&p);
    if (e == ESP_OK || e == ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGI(TAG, "Peer %-20s " MACSTR, label, MAC2STR(mac));
    } else {
        ESP_LOGE(TAG, "add_peer %s failed: %s", label, esp_err_to_name(e));
    }
}

esp_err_t espnow_init(void) {
    /* Queue must exist before the callback is registered */
    g_packet_queue = xQueueCreate(ESPNOW_QUEUE_LEN, sizeof(espnow_packet_t));
    if (!g_packet_queue) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) { ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err)); return err; }

    err = esp_now_register_recv_cb(espnow_recv_cb);
    if (err != ESP_OK) { ESP_LOGE(TAG, "register_recv_cb: %s", esp_err_to_name(err)); return err; }

    add_peer(g_node2_mac, "NODE_2 (Security)");
    add_peer(g_node3_mac, "NODE_3 (Environment)");
    add_peer(g_node4_mac, "NODE_4 (Vision)");

    ESP_LOGI(TAG, "ESP-NOW ready. Channel=%d", ESPNOW_CHANNEL);
    return ESP_OK;
}

esp_err_t espnow_send_cmd(const uint8_t *mac, int command, int userID, int location) {
    if (!mac) return ESP_ERR_INVALID_ARG;

    espnow_packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    strncpy(pkt.name, "NODE_1", sizeof(pkt.name) - 1);
    pkt.command   = command;
    pkt.userID    = userID;
    pkt.location  = location;
    pkt.timestamp = (unsigned long)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    esp_err_t err = esp_now_send(mac, (const uint8_t *)&pkt, sizeof(pkt));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "send cmd=%d -> " MACSTR " failed: %s",
                 command, MAC2STR(mac), esp_err_to_name(err));
    }
    return err;
}
