/* espnow.h  -  ESP-NOW layer (ESP-IDF 6.x) */
#pragma once
#include "esp_err.h"
#include "shared_types.h"
#include "freertos/FreeRTOS.h"  /* MUST come before any other FreeRTOS header */
#include "freertos/queue.h"
#include <stdint.h>

/* Initialise ESP-NOW and register peers (NODE_2, NODE_3, NODE_4).
   Must be called AFTER wifi_init_sta(). */
esp_err_t espnow_init(void);

/* FreeRTOS queue: recv callback -> worker task.
   Items are espnow_packet_t (42 bytes each). */
extern QueueHandle_t g_packet_queue;

/* Peer MAC addresses, filled by espnow_init(), used in main.c */
extern uint8_t g_node2_mac[6];  /* NODE_2 Security       F0:24:F9:0B:AA:D8 */
extern uint8_t g_node3_mac[6];  /* NODE_3 Environment    F0:24:F9:0C:8D:30 */
extern uint8_t g_node4_mac[6];  /* NODE_4 Vision/CAM     68:25:DD:2D:D7:1C */

/* Build and send a command packet to a peer.
   mac      - one of g_node2/3/4_mac
   command  - CMD_* from config.h
   userID   - only used for door-open commands to NODE_2
   location - only used for door-open commands to NODE_2 */
esp_err_t espnow_send_cmd(const uint8_t *mac, int command, int userID, int location);
