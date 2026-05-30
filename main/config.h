/*
 * config.h  —  NODE_1 Master (ESP32-S3, ESP-IDF 5.x)
 * Single source of truth for ALL credentials, MAC addresses,
 * virtual pins, command values, and task-sizing constants.
 * Fill in every field marked  <- FILL IN  before flashing.
 */
#pragma once
 
/* ---- WiFi ---- */
#define WIFI_SSID            "YOUR_WIFI_SSID"        /* <- FILL IN */
#define WIFI_PASSWORD        "YOUR_WIFI_PASSWORD"    /* <- FILL IN */
 
/* ---- Blynk IoT (cloud.blynk.cc) ---- */
#define BLYNK_AUTH_TOKEN     "YOUR_BLYNK_AUTH_TOKEN" /* <- FILL IN */
#define BLYNK_HOST           "blynk.cloud"
#define BLYNK_PORT           443
 
/* ---- Imgbb (imgbb.com -> API page) ---- */
#define IMGBB_API_KEY        "YOUR_IMGBB_API_KEY"    /* <- FILL IN */
#define IMGBB_UPLOAD_URL     "https://api.imgbb.com/1/upload"
 
/* ---- Face++ (faceplusplus.com) ----
 * Enrollment: Console -> FaceSet -> "HomeResidents"
 *   Upload 3-5 photos per person; user_id = "Caleb" / "Emmanuel" / "Mary" / "Princess"
 */
#define FACEPP_API_KEY        "YOUR_FACEPP_API_KEY"       /* <- FILL IN */
#define FACEPP_API_SECRET     "YOUR_FACEPP_API_SECRET"    /* <- FILL IN */
#define FACEPP_FACESET_TOKEN  "YOUR_FACEPP_FACESET_TOKEN" /* <- FILL IN */
#define FACEPP_SEARCH_URL     "https://api-us.faceplusplus.com/facepp/v3/search"
#define FACEPP_MIN_CONFIDENCE 75.0f
 
/* ---- NODE_4 camera ----
 * NODE_4 connects to NODE_1's soft-AP (SENTINEL-HUB) and uses the static
 * IP 192.168.4.100 (set in NODE_4.ino via WiFi.config). NODE_1's AP
 * gateway is 192.168.4.1 so NODE_1 reaches NODE_4 directly at this address.
 * Do NOT use a 192.168.1.x address — that is the home router subnet and is
 * unreachable from the SENTINEL-HUB AP interface.                         */
#define NODE4_IP          "192.168.4.100"
#define NODE4_CAPTURE_URL "http://" NODE4_IP "/capture"
 
/* ---- MAC Addresses (WiFi STA — confirmed from serial output) ----
 * NODE_1 (this device)  90:70:69:16:9A:FC
 * NODE_2 Security       EC:E3:34:9A:82:65
 * NODE_3 Environment    F0:24:F9:0C:8D:30
 * NODE_4 Vision/CAM     68:25:DD:2D:D7:1C
 */
#define NODE2_MAC  { 0xEC, 0xE3, 0x34, 0x9A, 0x82, 0x65 }
#define NODE3_MAC  { 0xF0, 0x24, 0xF9, 0x0C, 0x8D, 0x30 }
#define NODE4_MAC  { 0x68, 0x25, 0xDD, 0x2D, 0xD7, 0x1C }

/* ---- Global MAC address arrays (defined in main.cpp) ---- */
#ifdef __cplusplus
extern "C" {
#endif
extern uint8_t g_node2_mac[6];
extern uint8_t g_node3_mac[6];
extern uint8_t g_node4_mac[6];
#ifdef __cplusplus
}
#endif
 
/* ---- ESP-NOW channel (must match router AND all other nodes) ---- */
#define ESPNOW_CHANNEL  6
 
/* ---- Blynk Virtual Pins ---- */
#define VPIN_USER_ID        0   /* Int   last verified userID               */
#define VPIN_LOCATION       1   /* Int   last event location                */
#define VPIN_LPG            2   /* Float LPG ppm                            */
#define VPIN_CH4            3   /* Float CH4 ppm                            */
#define VPIN_SMOKE          4   /* Float Smoke ppm                          */
#define VPIN_HYDROGEN       5   /* Float H2 ppm                             */
#define VPIN_EVENT_LABEL    6   /* String event description                 */
#define VPIN_CAM_IMAGE      7   /* String Imgbb URL -> Blynk Image Widget   */
#define VPIN_NODE_NAME      8   /* String sender node name                  */
#define VPIN_GAS_ALERT      9   /* Int   1=gas danger LED                   */
#define VPIN_SEC_ALERT      10  /* Int   1=intruder LED                     */
#define VPIN_MANUAL_CAP     11  /* Button manual face verify                */
#define VPIN_FACE_RESULT    12  /* String face verify result                */
#define VPIN_DOOR_OVERRIDE  13  /* Button force main door open (NODE_2)     */
#define VPIN_VENT_OVERRIDE  14  /* Button force vent fan ON (NODE_3)        */
#define VPIN_VALVE_OVERRIDE 15  /* Button force solenoid valve (NODE_3)     */
#define VPIN_PIR_STATUS     16  /* Int   1=PIR active                       */
 
/* ---- Command values (command field of espnow_packet_t) ----
 * These must match the switch() cases in NODE_2, NODE_3, NODE_4.
 */
#define CMD_IDLE          0  /* Normal data packet                          */
#define CMD_START_STREAM  1  /* NODE_1->NODE_4: switch to JPEG + start HTTP */
#define CMD_STOP_STREAM   2  /* NODE_1->NODE_4: stop HTTP, back to detect   */
#define CMD_VENT_ON       3  /* NODE_1->NODE_3: force vent fan ON           */
#define CMD_VENT_OFF      4  /* NODE_1->NODE_3: force vent fan OFF          */
#define CMD_VALVE_ON      5  /* NODE_1->NODE_3: open solenoid valve         */
#define CMD_VALVE_OFF     6  /* NODE_1->NODE_3: close solenoid valve        */
#define CMD_DOOR_OPEN     7  /* NODE_1->NODE_2/3: open  servo               */
#define CMD_DOOR_CLOSE    8  /* NODE_1->NODE_2/3: close servo               */
#define CMD_SET_FLASH     9  /* NODE_1->NODE_4: LED brightness (0-255 in userID field) */
 
/* ---- Location values (location field of espnow_packet_t) ---- */
#define LOC_HEARTBEAT   0
#define LOC_GATE        1
#define LOC_MAIN_DOOR   2
#define LOC_INTRUDER    3  /* Gas danger, PIR, unknown RFID, or motion      */
#define LOC_BACK_DOOR   4  /* Back door — NODE_3 servo                      */
 
/* ---- Gas thresholds (ppm) ----
 * Alert fires at these levels. Dashboard shows amber warning at 60% of these.
 * Cross-sensitivity suppression in main.c: if gas is above threshold,
 * smoke/H2 only treated as real if it exceeds 2× its threshold.
 */
#define THRESHOLD_LPG    600.0f   /* was 1000 — reduced for earlier detection */
#define THRESHOLD_CH4    600.0f   /* was 1000                                  */
#define THRESHOLD_SMOKE  180.0f   /* was 300                                   */
#define THRESHOLD_H2     300.0f   /* was 500                                   */
 
/* ---- Task / Queue sizing ---- */
#define ESPNOW_QUEUE_LEN      10     /* Max queued packets before drop       */
#define WORKER_TASK_STACK     20480  /* 20 KB: TLS + cJSON + base64          */
#define WORKER_TASK_PRIORITY  5
#define WEBHOOK_SERVER_PORT   8080   /* Listens for Blynk button webhooks    */