/*
 * shared_types.h  —  espnow_packet_t  (NODE_1, ESP-IDF 5.x)
 * The struct layout MUST be byte-for-byte identical on all four nodes.
 * Arduino nodes typedef it as struct_message but the binary is the same.
 * Total: 42 bytes with __attribute__((packed)).
 */
#pragma once
#include <stdint.h>
#include <string.h>

typedef struct __attribute__((packed)) {
    char          name[10];   /* Sender e.g. "NODE_2"                        */
    int           userID;     /* 0=unknown 1=Caleb 2=Emmanuel 3=Mary 4=Princess */
    int           location;   /* 0=heartbeat 1=gate 2=door 3=intruder/danger */
    float         lpg;        /* LPG ppm                                     */
    float         CH4;        /* CH4 ppm                                     */
    float         smoke;      /* Smoke ppm                                   */
    float         hydrogen;   /* H2 ppm                                      */
    int           command;    /* CMD_* from config.h                         */
    unsigned long timestamp;  /* millis() on sender                          */
} espnow_packet_t;

/* userID -> name string */
static inline const char *user_name(int id) {
    switch (id) {
        case 1: return "Caleb";
        case 2: return "Emmanuel";
        case 3: return "Mary";
        case 4: return "Princess";
        default: return "Unknown";
    }
}

/* location -> description string */
static inline const char *location_name(int loc) {
    switch (loc) {
        case 1: return "Gate";
        case 2: return "Main Door";
        case 3: return "INTRUDER / Danger";
        default: return "Heartbeat";
    }
}

/* Face++ user_id label -> userID integer (case-insensitive) */
static inline int label_to_userid(const char *label) {
    if (!label || !label[0]) return 0;
    if (strcasecmp(label, "Caleb")    == 0) return 1;
    if (strcasecmp(label, "Emmanuel") == 0) return 2;
    if (strcasecmp(label, "Mary")     == 0) return 3;
    if (strcasecmp(label, "Princess") == 0) return 4;
    return 0;
}
