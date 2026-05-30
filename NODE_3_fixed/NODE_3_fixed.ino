/*
 * =============================================================
 *  NODE_3.ino  —  Environment Node  (ESP32, Arduino Core 3.x)
 * =============================================================
 *  Hardware:
 *    MQ-6  (pin 34)  → LPG + CH4 measurement
 *    MQ-2  (pin 35)  → Smoke + Hydrogen measurement
 *    4× PIR sensors  → Intruder zones (pins 13, 12, 14, 25)
 *    Solenoid valve  → Gas shutoff relay (pin 32, LOW=closed)
 *    Vent fan relay  → Ventilation (pin 33, LOW=off)
 *    Backdoor servo  → Emergency egress (pin 18)
 *
 *  MAC addresses:
 *    This node  (STA): F0:24:F9:0C:8D:30
 *    NODE_1 (master) : 90:70:69:16:9A:FC
 *
 *  ESP-NOW roles:
 *    SENDS  → NODE_1 : gas readings, PIR alerts, heartbeats
 *    RECEIVES ← NODE_1 : vent/valve override commands from Blynk
 *
 *  IMPORTANT:
 *    Run calibration in CLEAN AIR (no gas leaks, outdoors if possible).
 *    If R0 prints out of 0.1–200 range, check sensor wiring.
 *    ESPNOW_WIFI_CHANNEL must match your router and all other nodes.
 * =============================================================
 */

#include <Arduino.h>
#include "ESP32_NOW.h"          // Arduino wrapper — used for sending only
#include "WiFi.h"
#include "esp_now.h"            // IDF low-level — needed for receive callback
#include <MQUnifiedsensor.h>
#include <ESP32Servo.h>

/* ================================================================
   COMMAND + LOCATION CONSTANTS  (must match shared_types.h / config.h)
   ================================================================ */
#define CMD_IDLE        0
#define CMD_VENT_ON     3
#define CMD_VENT_OFF    4
#define CMD_VALVE_ON    5
#define CMD_VALVE_OFF   6
#define CMD_DOOR_OPEN   7
#define CMD_DOOR_CLOSE  8
#define LOC_HEARTBEAT   0
#define LOC_INTRUDER    3
#define LOC_BACK_DOOR   4
#define MQ_BOARD            "ESP32"
#define MQ_VOLTAGE          3.3f
#define MQ_ADC_BITS         12
#define MQ_CALIBRATION_RUNS 15   // Readings averaged for R0 (in clean air)

/* ================================================================
   PIN ASSIGNMENTS
   ================================================================ */
#define PIN_MQ6            34
#define PIN_MQ2            35
#define PIN_SOLENOID_VALVE 32    // HIGH = open (gas shutoff), LOW = closed in1
#define PIN_VENT_FAN       33    // HIGH = fan on,             LOW = off in2
#define PIN_SERVO_BACKDOOR 18
#define buzzer             19
static const uint8_t PIR_PINS[4] = {13, 26, 14, 25};

/* ================================================================
   TIMING & THRESHOLDS
   ================================================================ */
#define ESPNOW_WIFI_CHANNEL   6
#define HEARTBEAT_INTERVAL_MS 10000UL
#define GAS_SENSE_INTERVAL_MS 2000UL
#define PIR_RESEND_MS         5000UL    // Re-alert every 5 s while PIR active
#define PIR_CLEAR_DEBOUNCE_MS 2000UL   // PIR must be LOW for 2 s before "all clear"
#define SERVO_STEP_MS         15UL      // Non-blocking: 1° per 15 ms

// Gas danger levels in ppm — adjust after calibration
#define THRESHOLD_LPG   1000.0f
#define THRESHOLD_CH4   1000.0f
#define THRESHOLD_SMOKE  300.0f
#define THRESHOLD_H2     500.0f

/* ================================================================
   MAC ADDRESSES
   ================================================================ */
static uint8_t master_mac[] = {0x90, 0x70, 0x69, 0x16, 0x9A, 0xFC};

/* ================================================================
   SHARED PACKET STRUCTURE  (identical on all four nodes)
   ================================================================ */
typedef struct __attribute__((packed)) {
    char          name[10];
    int           userID;     // 0 on NODE_3 (no user concept here)
    int           location;   // 0=heartbeat  3=danger(gas/PIR)
    float         lpg;
    float         CH4;
    float         smoke;
    float         hydrogen;
    int           command;    // 0=idle  3=vent_on  4=vent_off  5=valve_on  6=valve_off
    unsigned long timestamp;
} struct_message;

/* ================================================================
   GAS SENSOR CALIBRATION CURVES
   Power-law model: ppm = a × (Rs/R0)^b
   Values from MQUnifiedsensor library documentation.
   ================================================================ */
struct GasCurve { float a; float b; };
static const GasCurve CURVE_MQ6_LPG   = {1009.2f,  -2.350f};
static const GasCurve CURVE_MQ6_CH4   = {2127.2f,  -2.526f};
static const GasCurve CURVE_MQ2_SMOKE = {36974.0f, -3.109f};
static const GasCurve CURVE_MQ2_H2    = {987.99f,  -2.162f};

static MQUnifiedsensor MQ6(MQ_BOARD, MQ_VOLTAGE, MQ_ADC_BITS, PIN_MQ6, "MQ-6");
static MQUnifiedsensor MQ2(MQ_BOARD, MQ_VOLTAGE, MQ_ADC_BITS, PIN_MQ2, "MQ-2");

/* ================================================================
   ESP-NOW PEER  (sending only)
   ================================================================ */
class MasterPeer : public ESP_NOW_Peer {
public:
    MasterPeer(const uint8_t *mac, uint8_t ch)
        : ESP_NOW_Peer(mac, ch, WIFI_IF_STA, nullptr) {}
    bool init()                                     { return add(); }
    bool sendMessage(const uint8_t *d, size_t len) { return send(d, len); }
};

static MasterPeer   *masterNode = nullptr;
static struct_message myData;   // Outgoing packet

/* ================================================================
   GAS DANGER STATE — file-scope so both gas logic and remote
   command handler can read/write it consistently.
   ================================================================ */
static bool gasDangerActive = false;

/* ================================================================
   BACKDOOR SERVO — non-blocking state machine
   servoTarget is set by gas logic; tickServo() moves toward it.
   ================================================================ */
static Servo         backdoor;
static int           servoTarget  = 0;
static int           servoCurrent = 0;
static unsigned long servoLastMs  = 0;

static void tickServo() {
    if (millis() - servoLastMs < SERVO_STEP_MS) return;
    servoLastMs = millis();
    if      (servoCurrent < servoTarget) { servoCurrent++; backdoor.write(servoCurrent); }
    else if (servoCurrent > servoTarget) { servoCurrent--; backdoor.write(servoCurrent); }
}

/* ================================================================
   RECEIVE CALLBACK  (Core 3.x IDF low-level API)
   NODE_1 sends remote control commands here (Blynk override).
   Runs in WiFi task → must be short; use flag+copy pattern.
   ================================================================ */
static volatile bool   remoteCommandReady = false;
static struct_message  remoteCmd;
static portMUX_TYPE    cmdMux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR onDataReceived(const esp_now_recv_info_t *info,
                                      const uint8_t            *data,
                                      int                       len) {
    if (!info || !data || len < (int)sizeof(struct_message)) return;
    if (memcmp(info->src_addr, master_mac, 6) != 0)          return; // Ignore non-master

    taskENTER_CRITICAL(&cmdMux);
    memcpy(&remoteCmd, data, sizeof(struct_message));
    remoteCommandReady = true;
    taskEXIT_CRITICAL(&cmdMux);
}

/* ================================================================
   SENSOR CALIBRATION HELPER
   Must be called in clean air. Averages MQ_CALIBRATION_RUNS
   readings and calls setR0() on the sensor object.
   ================================================================ */
// Change the function signature to accept the ratio
static float calibrateSensor(MQUnifiedsensor &sensor, float ratioInCleanAir, const char *label) {
    Serial.printf("[CAL] Calibrating %s — ensure clean air...\n", label);
    float r0Sum = 0;
    for (int i = 0; i < MQ_CALIBRATION_RUNS; i++) {
        sensor.update();
        // Pass the ratio into the calibrate method
        r0Sum += sensor.calibrate(ratioInCleanAir);
        delay(500);
        Serial.print(".");
    }
    Serial.println();
    float r0 = r0Sum / MQ_CALIBRATION_RUNS;
    sensor.setR0(r0);
    Serial.printf("[CAL] %s R0 = %.4f\n", label, r0);

    // Sanity check
    if (r0 < 0.1f || r0 > 200.0f) {
        Serial.printf("[WARN] %s R0 out of expected range (0.1–200). Check wiring!\n", label);
    }
    return r0;
}

/* ================================================================
   SETUP
   ================================================================ */
void setup() {
    Serial.begin(115200);
    Serial.println("\n[BOOT] NODE_3 Environment Node starting...");
    Serial.printf("[INFO] This node MAC: F0:24:F9:0C:8D:30\n");
    Serial.printf("[INFO] Master (NODE_1) MAC: 90:70:69:16:9A:FC\n");

    // Output pins — safe defaults (everything OFF on boot)
    pinMode(PIN_SOLENOID_VALVE, OUTPUT);
    pinMode(PIN_VENT_FAN,       OUTPUT);
    digitalWrite(PIN_SOLENOID_VALVE,HIGH );  // Valve closed
    digitalWrite(PIN_VENT_FAN,       LOW);  // Fan off
    pinMode(buzzer, OUTPUT);                // Buzzer as output
    digitalWrite(buzzer, LOW);

    // PIR input pins — INPUT_PULLDOWN prevents floating pins from
    // reading HIGH when no sensor is connected or wiring is loose.
    // HC-SR501 output is actively driven HIGH/LOW so the pull-down
    // does not interfere with real sensor readings.
    for (int i = 0; i < 4; i++) {
        pinMode(PIR_PINS[i], INPUT_PULLDOWN);
    }

    // Backdoor servo
    backdoor.attach(PIN_SERVO_BACKDOOR);
    backdoor.write(0);

    // WiFi radio on (needed for ESP-NOW) but NOT connected to an AP
    WiFi.mode(WIFI_STA);
    WiFi.setChannel(ESPNOW_WIFI_CHANNEL);

    if (!ESP_NOW.begin()) {
        Serial.println("[ERROR] ESP-NOW init failed — halting.");
        while (1) delay(1000);
    }

    masterNode = new MasterPeer(master_mac, ESPNOW_WIFI_CHANNEL);
    if (!masterNode->init()) {
        Serial.println("[ERROR] Failed to add NODE_1 peer — halting.");
        while (1) delay(1000);
    }

    // Receive callback for Blynk override commands from NODE_1
    esp_err_t cbErr = esp_now_register_recv_cb(onDataReceived);
    if (cbErr != ESP_OK) {
        Serial.printf("[WARN] recv_cb register failed: 0x%02X\n", cbErr);
    } else {
        Serial.println("[OK] Receive callback registered.");
    }

    // MQ sensor initialisation + R0 calibration
    // setRegressionMethod(1) = exponential regression (recommended for these sensors)
    // MQ sensor initialisation + R0 calibration
    MQ6.init();
    MQ6.setRegressionMethod(1);
    MQ6.setA(CURVE_MQ6_LPG.a);
    MQ6.setB(CURVE_MQ6_LPG.b);

    MQ2.init();
    MQ2.setRegressionMethod(1);
    MQ2.setA(CURVE_MQ2_SMOKE.a);
    MQ2.setB(CURVE_MQ2_SMOKE.b);

    /* MQ sensors require a heating period before their resistance
     * stabilises. Without this, R0 is near 0 → ppm = inf or nan.
     * MQ-2 and MQ-6 datasheets specify ≥30 s preheat.             */
    Serial.println("[CAL] Preheating MQ sensors — 45 seconds...");
    for (int i = 45; i > 0; i--) {
        MQ6.update();
        MQ2.update();
        if (i % 5 == 0) Serial.printf("[CAL] %d s remaining...\n", i);
        delay(1000);
    }
    Serial.println("[CAL] Preheat complete — calibrating R0 in clean air.");

    calibrateSensor(MQ6, 10.0f, "MQ-6"); // <--- Added 10.0f here
    calibrateSensor(MQ2, 9.83f, "MQ-2"); // <--- Added 9.83f here
    // Initialise outgoing packet
    memset(&myData, 0, sizeof(myData));
    strncpy(myData.name, "NODE_3", sizeof(myData.name) - 1);

    Serial.println("[OK] NODE_3 ready.\n");
}

/* ================================================================
   LOOP
   ================================================================ */
void loop() {
    myData.timestamp = millis();

    // ---- 1. Advance backdoor servo one step (non-blocking) ----
    tickServo();

    // ---- 2. Handle remote commands from NODE_1 (Blynk override) ----
    if (remoteCommandReady) {
        struct_message cmd;
        taskENTER_CRITICAL(&cmdMux);
        memcpy(&cmd, &remoteCmd, sizeof(cmd));
        remoteCommandReady = false;
        taskEXIT_CRITICAL(&cmdMux);

        if (strncmp(cmd.name, "NODE_1", 6) == 0) {
            switch (cmd.command) {
                case CMD_VENT_ON:
                    digitalWrite(PIN_VENT_FAN, HIGH);
                    Serial.println("[REMOTE] Vent fan ON (dashboard override).");
                    break;
                case CMD_VENT_OFF:
                    /* Only stop fan if gas is not currently active */
                    if (!gasDangerActive) {
                        digitalWrite(PIN_VENT_FAN, LOW);
                        Serial.println("[REMOTE] Vent fan OFF (dashboard override).");
                    } else {
                        Serial.println("[REMOTE] Vent OFF blocked — gas alert still active.");
                    }
                    break;
                case CMD_VALVE_ON:
                    digitalWrite(PIN_SOLENOID_VALVE, HIGH);
                    Serial.println("[REMOTE] Solenoid valve OPEN (dashboard override).");
                    break;
                case CMD_VALVE_OFF:
                    digitalWrite(PIN_SOLENOID_VALVE, LOW);
                    Serial.println("[REMOTE] Solenoid valve CLOSED (dashboard override).");
                    break;
                case CMD_DOOR_OPEN:
                    if (cmd.location == LOC_BACK_DOOR) {
                        servoTarget = 90;
                        Serial.println("[REMOTE] Back door OPEN (dashboard).");
                    }
                    break;
                case CMD_DOOR_CLOSE:
                    if (cmd.location == LOC_BACK_DOOR) {
                        servoTarget = 0;
                        Serial.println("[REMOTE] Back door CLOSE (dashboard).");
                    }
                    break;
                default:
                    break;
            }
        }
    }

    // ---- 3. PIR intruder detection (runs every loop — fast read) ----
    //
    // FIX: Added PIR_CLEAR_DEBOUNCE_MS before declaring "all clear".
    //
    // Previous bug: clearing pirLastLocation to 0 on ANY LOW read meant
    // the very next HIGH was always stateChange=true, sending a new alert
    // immediately — completely bypassing PIR_RESEND_MS. With a bouncing
    // or floating pin this produced thousands of alerts per second.
    //
    // Fix A: INPUT_PULLDOWN (in setup) stops floating pins reading HIGH.
    // Fix B: PIR must stay LOW for PIR_CLEAR_DEBOUNCE_MS before state
    //        resets to 0. Transient LOW blips no longer reset the state.
    static int           pirLastLocation   = -1;
    static unsigned long pirLastSendMs     = 0;
    static bool          pirActive         = false;
    static bool          pirClearPending   = false;
    static unsigned long pirClearStartMs   = 0;

    {
        int activeZones = 0;
        for (int i = 0; i < 4; i++) {
            if (digitalRead(PIR_PINS[i]) == HIGH) activeZones++;
        }

        bool newPirActive = (activeZones > 0);

        if (newPirActive) {
            pirClearPending = false;   // Cancel any in-progress clear debounce
            pirActive = true;

            // Send on first trigger OR every PIR_RESEND_MS while still active.
            // stateChange is only true when coming from a confirmed clear
            // (pirLastLocation == 0), not from a transient LOW blip.
            bool stateChange = (pirLastLocation != 3);
            bool resendDue   = (millis() - pirLastSendMs > PIR_RESEND_MS);

            if (stateChange || resendDue) {
                struct_message pkt;
                memcpy(&pkt, &myData, sizeof(pkt));
                pkt.location  = 3;
                pkt.userID    = 0;
                pkt.command   = 0;
                pkt.timestamp = millis();
                bool ok = masterNode->sendMessage((uint8_t *)&pkt, sizeof(pkt));
                if (ok) pirLastSendMs = millis();
                Serial.printf("[PIR] %d zone(s) active — %s\n",
                              activeZones, ok ? "alert sent" : "send failed");
                pirLastLocation = 3;
            }
        } else {
            // All zones LOW — start (or continue) the clear debounce timer.
            // Only declare "all clear" after PIR_CLEAR_DEBOUNCE_MS of sustained LOW.
            if (pirLastLocation == 3) {
                if (!pirClearPending) {
                    pirClearPending = true;
                    pirClearStartMs = millis();
                } else if (millis() - pirClearStartMs >= PIR_CLEAR_DEBOUNCE_MS) {
                    pirClearPending = false;
                    pirActive       = false;
                    struct_message pkt;
                    memcpy(&pkt, &myData, sizeof(pkt));
                    pkt.location  = 0;
                    pkt.userID    = 0;
                    pkt.command   = 0;
                    pkt.timestamp = millis();
                    masterNode->sendMessage((uint8_t *)&pkt, sizeof(pkt));
                    Serial.println("[PIR] All zones clear (debounce passed).");
                    pirLastLocation = 0;
                }
            }
        }
    }

    // ---- 4. Gas sensing every GAS_SENSE_INTERVAL_MS ----
    //
    // BUG FIX: Gas danger state and PIR state are tracked separately
    // with dedicated boolean flags. Previously both used myData.location==3
    // which caused gas-clear logic to fail if PIR was also active, and
    // vice versa.
    static unsigned long lastGasRead    = 0;
    // gasDangerActive is now file-scope (declared above loop)

    if (millis() - lastGasRead >= GAS_SENSE_INTERVAL_MS) {
        lastGasRead = millis();

        // update() reads the raw ADC value into the sensor object.
        // Must be called before each readSensor() call.
        // Reading two gases from one sensor: call setA/setB then readSensor() twice.
        MQ6.update();
        MQ6.setA(CURVE_MQ6_LPG.a);  MQ6.setB(CURVE_MQ6_LPG.b);
        myData.lpg  = MQ6.readSensor();

        MQ6.setA(CURVE_MQ6_CH4.a);  MQ6.setB(CURVE_MQ6_CH4.b);
        myData.CH4  = MQ6.readSensor();

        MQ2.update();
        MQ2.setA(CURVE_MQ2_SMOKE.a); MQ2.setB(CURVE_MQ2_SMOKE.b);
        myData.smoke = MQ2.readSensor();

        MQ2.setA(CURVE_MQ2_H2.a);   MQ2.setB(CURVE_MQ2_H2.b);
        myData.hydrogen = MQ2.readSensor();

        Serial.printf("[GAS] LPG=%.1f CH4=%.1f Smoke=%.1f H2=%.1f ppm\n",
                      myData.lpg, myData.CH4, myData.smoke, myData.hydrogen);

        /* Guard: replace nan/inf (caused by sensor not yet warmed up or
         * bad wiring) with 0 so NODE_1 dashboard shows 0 instead of
         * triggering false alarms or crashing snprintf with %f=nan.  */
        if (!isfinite(myData.lpg))      myData.lpg      = 0.0f;
        if (!isfinite(myData.CH4))      myData.CH4      = 0.0f;
        if (!isfinite(myData.smoke))    myData.smoke    = 0.0f;
        if (!isfinite(myData.hydrogen)) myData.hydrogen = 0.0f;

        bool isDangerous = (myData.lpg      > THRESHOLD_LPG   ||
                            myData.CH4      > THRESHOLD_CH4   ||
                            myData.smoke    > THRESHOLD_SMOKE  ||
                            myData.hydrogen > THRESHOLD_H2);

if (isDangerous) {
            // TRIGGER ALARM: Immediate response
            if (!gasDangerActive) {
                gasDangerActive = true;
                Serial.println("[ALERT] Gas danger detected! Initiating safety protocol.");
            }
            
            digitalWrite(PIN_VENT_FAN, HIGH);        // Fan ON
            digitalWrite(PIN_SOLENOID_VALVE, LOW);   // Valve CLOSED (Fail-safe)
            servoTarget = 90;                        // Backdoor OPEN
            tone(buzzer, 900);
        } 
        else if (gasDangerActive) {
            // RECOVERY: Gas is now safe, but check PIR before resetting hardware
            if (!pirActive) {
                gasDangerActive = false;
                digitalWrite(PIN_VENT_FAN, LOW);         // Fan OFF
                digitalWrite(PIN_SOLENOID_VALVE, HIGH);  // Valve OPEN
                servoTarget = 0;  
                noTone(buzzer);         // Stop the sound                       // Backdoor CLOSED
                Serial.println("[OK] Gas levels safe and room empty. System reset.");
            } else {
                // Latch state: Gas is clear, but keep ventilation for safety while movement exists
                Serial.println("[INFO] Gas cleared, but movement detected. Maintaining ventilation.");
            }
        }

        // Send gas data packet to NODE_1 regardless of danger state
        // (NODE_1 needs the readings for Blynk dashboard gauges)
        myData.location = isDangerous ? 3 : 0;
        myData.userID   = 0;
        myData.command  = 0;
        bool ok = masterNode->sendMessage((uint8_t *)&myData, sizeof(myData));
        if (!ok) Serial.println("[WARN] Gas packet send failed.");
    }

    // ---- 5. Heartbeat (10 s keep-alive when not in alarm state) ----
    static unsigned long lastHB = 0;
    if (millis() - lastHB >= HEARTBEAT_INTERVAL_MS) {
        // Gas packets already serve as keep-alive during an alarm.
        // Only send a dedicated heartbeat when everything is quiet.
        bool inAlarm = (myData.lpg      > THRESHOLD_LPG   ||
                        myData.CH4      > THRESHOLD_CH4   ||
                        myData.smoke    > THRESHOLD_SMOKE  ||
                        myData.hydrogen > THRESHOLD_H2    ||
                        pirActive);

        if (!inAlarm) {
            struct_message hb;
            memcpy(&hb, &myData, sizeof(hb));
            hb.location  = 0;   // heartbeat
            hb.userID    = 0;
            hb.command   = 0;
            hb.timestamp = millis();
            bool ok = masterNode->sendMessage((uint8_t *)&hb, sizeof(hb));
            if (ok) lastHB = millis();
            else    Serial.println("[WARN] Heartbeat send failed.");
        } else {
            // Advance timer anyway so we don't burst-send heartbeats on clear
            lastHB = millis();
        }
    }
}
