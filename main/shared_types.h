/*
 * shared_types.h  —  espnow_packet_t + all command/location constants
 *
 * CRITICAL: The struct layout MUST be byte-for-byte identical on ALL
 * four nodes. Arduino nodes typedef it as struct_message but the
 * binary layout is the same. Total: 42 bytes with packed attribute.
 *
 * This file is included by:
 *   NODE_1  (ESP-IDF)    — #include "shared_types.h"
 *   NODE_2  (Arduino)    — copy typedef as struct_message (same layout)
 *   NODE_3  (Arduino)    — same
 *   NODE_4  (Arduino)    — same
 *
 * CHANGES vs previous version:
 *   - All CMD_* constants moved here from config.h so every node
 *     uses the same values without duplication.
 *   - LOC_BACK_DOOR (4) added for NODE_3 back door servo.
 *   - location_name() updated to include LOC_BACK_DOOR.
 *   - CMD_DOOR_OPEN, CMD_DOOR_CLOSE, CMD_VENT_OFF, CMD_VALVE_OFF added.
 */
#pragma once
#include <stdint.h>
#include <string.h>
 
/* ================================================================
   COMMAND CONSTANTS  (command field in espnow_packet_t)
   ================================================================ */
#define CMD_IDLE          0   /* Heartbeat / no action                      */
#define CMD_START_STREAM  1   /* NODE_1 → NODE_4: start JPEG HTTP server    */
#define CMD_STOP_STREAM   2   /* NODE_1 → NODE_4: stop  JPEG HTTP server    */
#define CMD_VENT_ON       3   /* NODE_1 → NODE_3: turn exhaust fan ON       */
#define CMD_VENT_OFF      4   /* NODE_1 → NODE_3: turn exhaust fan OFF      */
#define CMD_VALVE_ON      5   /* NODE_1 → NODE_3: open  solenoid valve      */
#define CMD_VALVE_OFF     6   /* NODE_1 → NODE_3: close solenoid valve      */
#define CMD_DOOR_OPEN     7   /* NODE_1 → NODE_2/3: open  servo             */
#define CMD_DOOR_CLOSE    8   /* NODE_1 → NODE_2/3: close servo             */
 
/* ================================================================
   LOCATION CONSTANTS  (location field in espnow_packet_t)
   ================================================================ */
#define LOC_HEARTBEAT  0   /* Periodic keepalive — no alert                 */
#define LOC_GATE       1   /* Main gate (NODE_2 RFID servo)                 */
#define LOC_MAIN_DOOR  2   /* Main door (NODE_2 fingerprint servo)          */
#define LOC_INTRUDER   3   /* Intruder alert OR gas/hazard danger           */
#define LOC_BACK_DOOR  4   /* Back door (NODE_3 servo)                      */
 
/* ================================================================
   PACKET STRUCTURE
   42 bytes packed — do not add or reorder fields without updating
   all four nodes simultaneously.
   ================================================================ */
typedef struct __attribute__((packed)) {
    char          name[10];   /* Sender ID e.g. "NODE_2"                    */
    int           userID;     /* 0=unknown 1=Caleb 2=Emmanuel 3=Mary 4=Princess */
    int           location;   /* LOC_* above                                */
    float         lpg;        /* LPG ppm  (NODE_3 only, others send 0)      */
    float         CH4;        /* CH4 ppm  (NODE_3 only, others send 0)      */
    float         smoke;      /* Smoke ppm(NODE_3 only, others send 0)      */
    float         hydrogen;   /* H2  ppm  (NODE_3 only, others send 0)      */
    int           command;    /* CMD_* above                                 */
    unsigned long timestamp;  /* millis() on sender                         */
} espnow_packet_t;
 
/* ================================================================
   HELPER FUNCTIONS  (static inline — safe to include everywhere)
   ================================================================ */
 
/* userID → display name */
static inline const char *user_name(int id) {
    switch (id) {
        case 1:  return "Caleb";
        case 2:  return "Emmanuel";
        case 3:  return "Mary";
        case 4:  return "Princess";
        default: return "Unknown";
    }
}
 
/* location → display string */
static inline const char *location_name(int loc) {
    switch (loc) {
        case LOC_GATE:       return "Gate";
        case LOC_MAIN_DOOR:  return "Main Door";
        case LOC_INTRUDER:   return "INTRUDER / Danger";
        case LOC_BACK_DOOR:  return "Back Door";
        default:             return "Heartbeat";
    }
}
 
/* Face++ identity label → userID integer */
static inline int label_to_userid(const char *label) {
    if (!label || !label[0]) return 0;
    if (strcasecmp(label, "Caleb")    == 0) return 1;
    if (strcasecmp(label, "Emmanuel") == 0) return 2;
    if (strcasecmp(label, "Mary")     == 0) return 3;
    if (strcasecmp(label, "Princess") == 0) return 4;
    return 0;
}
 