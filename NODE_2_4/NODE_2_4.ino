/*
 * =============================================================
 *  NODE_2.ino  —  Security Node  (ESP32, Arduino Core 3.x)
 * =============================================================
 *  Hardware:
 *    MFRC522 RFID reader  → gate access (SPI on pins 5/4/18/19/23)
 *    Adafruit fingerprint → main door access (UART2 on pins 16/17)
 *    Servo 1 (pin 13)     → Main Gate
 *    Servo 2 (pin 27)     → Main Door
 *
 *  MAC addresses:
 *    This node  (STA): F0:24:F9:0B:AA:D8
 *    NODE_1 (master) : 90:70:69:16:9A:FC
 *
 *  ESP-NOW roles:
 *    SENDS  → NODE_1 : access events, intruder alerts, heartbeats
 *    RECEIVES ← NODE_1 : door-open commands after face verification
 *
 *  IMPORTANT:
 *    ESPNOW_WIFI_CHANNEL (6) must match your router's WiFi channel
 *    AND must match all other nodes. Change it if your router uses
 *    a different channel.
 * =============================================================
 */

#include <Arduino.h>
#include "ESP32_NOW.h"
#include "WiFi.h"
#include "esp_now.h"
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Adafruit_Fingerprint.h>
#include "soc/soc.h"           // Brownout disable
#include "soc/rtc_cntl_reg.h"  // Brownout disable

/* ================================================================
   PIN ASSIGNMENTS
   ================================================================ */
#define SS_PIN      5//SDA
#define RST_PIN     4
#define SCK_PIN     18
#define MISO_PIN    19
#define MOSI_PIN    23
#define SERVO_PIN_1 13
#define SERVO_PIN_2 27

/* ================================================================
   TIMING CONSTANTS
   ================================================================ */
#define ESPNOW_WIFI_CHANNEL   6
#define HEARTBEAT_INTERVAL_MS 10000UL
#define FINGER_POLL_MS        200UL
#define SERVO_STEP_MS         15UL
#define DOOR_HOLD_MS          5000UL
#define INTRUDER_DEBOUNCE_MS  5000UL

/* ================================================================
   COMMAND + LOCATION CONSTANTS  (must match shared_types.h / config.h)
   ================================================================ */
#define CMD_IDLE        0
#define CMD_DOOR_OPEN   7
#define CMD_DOOR_CLOSE  8
#define LOC_HEARTBEAT   0
#define LOC_GATE        1
#define LOC_MAIN_DOOR   2
#define LOC_INTRUDER    3

/* ================================================================
   MAC ADDRESSES
   ================================================================ */
static uint8_t master_mac[] = {0x90, 0x70, 0x69, 0x16, 0x9A, 0xFC};

/* ================================================================
   AUTHORISED RFID CARD UIDs
   ================================================================ */
static const byte CARD_CALEB[]    = {0x33, 0xC0, 0xEE, 0x05};
static const byte CARD_EMMANUEL[] = {0xAE, 0x60, 0x6A, 0x06};
static const byte CARD_MARY[]     = {0xBF, 0xEA, 0x58, 0x86};
static const byte CARD_PRINCESS[] = {0x51, 0xCA, 0x45, 0x6A};

/* ================================================================
   SHARED PACKET STRUCTURE
   ================================================================ */
typedef struct __attribute__((packed)) {
    char          name[10];
    int           userID;
    int           location;
    float         lpg;
    float         CH4;
    float         smoke;
    float         hydrogen;
    int           command;
    unsigned long timestamp;
} struct_message;

/* ================================================================
   ESP-NOW PEER CLASS
   ================================================================ */
class MasterPeer : public ESP_NOW_Peer {
public:
    MasterPeer(const uint8_t *mac, uint8_t ch)
        : ESP_NOW_Peer(mac, ch, WIFI_IF_STA, nullptr) {}

    bool init()                                     { return add(); }
    bool sendMessage(const uint8_t *d, size_t len) { return send(d, len); }
};

/* ================================================================
   GLOBAL OBJECTS
   ================================================================ */
static struct_message  myData;
static MasterPeer     *masterNode = nullptr;
static MFRC522         mfrc522(SS_PIN, RST_PIN);
static HardwareSerial  mySerial(2);
static Adafruit_Fingerprint finger(&mySerial);
static Servo           main_gate;
static Servo           main_door;
static bool            fingerOK = false; // set true in setup() once sensor verifies

/* ================================================================
   NON-BLOCKING SERVO STATE MACHINE
   ================================================================ */
struct ServoState {
    Servo        &servo;
    int           current;
    int           target;
    unsigned long lastStep;
    unsigned long holdUntil;
    bool          holding;
};

static ServoState gateState = {main_gate, 0, 0, 0, 0, false};
static ServoState doorState = {main_door, 0, 0, 0, 0, false};

static void tickServo(ServoState &s) {
    unsigned long now = millis();
    if (s.holding && now >= s.holdUntil) {
        s.holding = false;
        s.target  = 0;
    }
    if (now - s.lastStep < SERVO_STEP_MS) return;
    s.lastStep = now;
    if      (s.current < s.target) { s.current++; s.servo.write(s.current); }
    else if (s.current > s.target) { s.current--; s.servo.write(s.current); }
}

static void openServo(ServoState &s) {
    s.target    = 90;
    s.holding   = true;
    s.holdUntil = millis() + DOOR_HOLD_MS;
}

/* closeServo() — immediately cancels hold and drives servo back to 0°.
   Called by dashboard "Close" button via CMD_DOOR_CLOSE from NODE_1. */
static void closeServo(ServoState &s) {
    s.target  = 0;
    s.holding = false;
}

/* ================================================================
   RECEIVE CALLBACK
   ================================================================ */
static volatile bool   remoteCommandReady = false;
static struct_message  remoteCmd;
static portMUX_TYPE    cmdMux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR onDataReceived(const esp_now_recv_info_t *info,
                                      const uint8_t            *data,
                                      int                       len) {
    if (!info || !data || len < (int)sizeof(struct_message)) return;
    if (memcmp(info->src_addr, master_mac, 6) != 0) return;
    taskENTER_CRITICAL(&cmdMux);
    memcpy(&remoteCmd, data, sizeof(struct_message));
    remoteCommandReady = true;
    taskEXIT_CRITICAL(&cmdMux);
}

/* ================================================================
   FINGERPRINT HELPER
   ================================================================ */
static int getFingerprintID() {
    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) return 0;
    if (p != FINGERPRINT_OK)       return -1;

    p = finger.image2Tz();
    if (p != FINGERPRINT_OK) return -1;

    p = finger.fingerSearch();
    if (p != FINGERPRINT_OK) return -1;

    return finger.fingerID;
}

/* ================================================================
   ACCESS HANDLER
   ================================================================ */
static void handleAccess() {
    const char *loc = (myData.location == 1) ? "GATE" : "MAIN DOOR";
    Serial.printf("[ACCESS] User %d granted at %s\n", myData.userID, loc);

    myData.command   = 0;
    myData.timestamp = millis();
    bool ok = masterNode->sendMessage((uint8_t *)&myData, sizeof(myData));
    if (!ok) Serial.println("[WARN] Access sendMessage failed.");

    if      (myData.location == 1) openServo(gateState);
    else if (myData.location == 2) openServo(doorState);
}

/* ================================================================
   SETUP
   ================================================================ */
void setup() {
    // ----------------------------------------------------------------
    // FIX: Disable brownout detector immediately.
    // Two servos + ESP-NOW radio firing simultaneously at boot can draw
    // 1A+, drooping the rail below the ~2.45V BOD threshold and causing
    // the reboot loop seen in serial logs. Hardware fix (bulk capacitance
    // on the 3.3V rail, dedicated servo power rail) is preferred long-term;
    // this prevents the reboot loop in the meantime.
    // ----------------------------------------------------------------
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

    Serial.begin(115200);
    delay(500);   // Give Serial time to connect before first print
    Serial.println("\n[BOOT] NODE_2 Security Node starting...");
    Serial.printf("[INFO] This node MAC: F0:24:F9:0B:AA:D8\n");
    Serial.printf("[INFO] Master (NODE_1) MAC: 90:70:69:16:9A:FC\n");

    // ----------------------------------------------------------------
    // FIX: Explicitly deassert SS before SPI init.
    // GPIO 5 is a strapping pin on ESP32 — if it floats LOW during boot
    // it can corrupt the boot mode. Driving it HIGH here before SPI.begin()
    // also ensures the MFRC522 CS is not accidentally asserted during init.
    // ----------------------------------------------------------------
    pinMode(SS_PIN, OUTPUT);
    digitalWrite(SS_PIN, HIGH);
    delay(50);

    // WiFi + ESP-NOW
    WiFi.mode(WIFI_STA);
    WiFi.setChannel(ESPNOW_WIFI_CHANNEL);

    if (!ESP_NOW.begin()) {
        Serial.println("[ERROR] ESP-NOW init failed — halting.");
        while (1) delay(1000);
    }

    masterNode = new MasterPeer(master_mac, ESPNOW_WIFI_CHANNEL);
    if (!masterNode->init()) {
        Serial.println("[WARN] Failed to add master peer.");
    }

    esp_err_t cbErr = esp_now_register_recv_cb(onDataReceived);
    if (cbErr != ESP_OK) {
        Serial.printf("[WARN] recv_cb register failed: 0x%02X\n", cbErr);
    } else {
        Serial.println("[OK] Receive callback registered.");
    }

    // ----------------------------------------------------------------
    // RFID init with firmware version diagnostic
    // Expected values: 0x91 or 0x92 = MFRC522 working correctly
    //                  0x00         = MISO stuck low (check MISO/MOSI swap)
    //                  0xFF         = SS not pulling low / no power
    //                  other        = wrong SPI pins or 5V on 3.3V line
    // ----------------------------------------------------------------
    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, SS_PIN);
    // FIX: Hard-reset the MFRC522 before PCD_Init. On Arduino-ESP32 3.x,
    // SPI.begin() with explicit pins can leave the peripheral in a
    // half-initialised state. Pulsing RST ensures a clean start.
    pinMode(RST_PIN, OUTPUT);
    digitalWrite(RST_PIN, LOW);
    delay(10);
    digitalWrite(RST_PIN, HIGH);
    delay(50);  // MFRC522 needs ≥37ms after reset before first command
    mfrc522.PCD_Init();
    // Boost antenna gain to maximum — improves card detection range.
    // Default gain (RxGain_48dB) is often too low for reliable reads.
    mfrc522.PCD_SetAntennaGain(MFRC522::RxGain_max);
    delay(50);  // Give MFRC522 time to settle after reset pulse

    byte rfidVersion = mfrc522.PCD_ReadRegister(MFRC522::VersionReg);
    Serial.printf("[RFID] Firmware version register: 0x%02X\n", rfidVersion);

    if (rfidVersion == 0x00) {
        Serial.println("[ERROR] RFID returned 0x00 — MISO/MOSI may be swapped, or no power.");
        Serial.println("        Check: MISO→GPIO19, MOSI→GPIO23, SCK→GPIO18, SS→GPIO5, RST→GPIO4");
        Serial.println("        Continuing anyway — RFID will not work until wiring is fixed.");
    } else if (rfidVersion == 0xFF) {
        Serial.println("[ERROR] RFID returned 0xFF — SS pin not pulling low, or 3.3V missing.");
        Serial.println("        Check: SS→GPIO5 with 3.3V supply (NOT 5V). RST→GPIO4.");
        Serial.println("        Continuing anyway — RFID will not work until wiring is fixed.");
    } else if (rfidVersion == 0x91 || rfidVersion == 0x92) {
        Serial.printf("[OK] RFID reader ready. (MFRC522 v%d.0)\n",
                      rfidVersion == 0x91 ? 1 : 2);
    } else {
        Serial.printf("[WARN] RFID returned unexpected version 0x%02X — may still work.\n",
                      rfidVersion);
    }

    // ----------------------------------------------------------------
    // RFID scan-present diagnostic: read the reader's own status
    // ComIEnReg and ErrorReg to confirm the antenna is energised.
    // ----------------------------------------------------------------
    byte comIEn  = mfrc522.PCD_ReadRegister(MFRC522::ComIEnReg);
    byte errorReg = mfrc522.PCD_ReadRegister(MFRC522::ErrorReg);
    Serial.printf("[RFID] ComIEnReg=0x%02X  ErrorReg=0x%02X\n", comIEn, errorReg);
    if (errorReg != 0x00) {
        Serial.println("[WARN] RFID ErrorReg non-zero — antenna may not be energised.");
        Serial.println("       Run mfrc522.PCD_SetAntennaGain(MFRC522::RxGain_max) if needed.");
    }

    // Fingerprint sensor — UART2
    // Wiring check: sensor TX → GPIO16 (ESP32 RX2), sensor RX → GPIO17 (ESP32 TX2)
    // ----------------------------------------------------------------
    // DIAGNOSTIC: send a raw getImage packet and print the exact byte
    // the sensor returns. This tells us whether UART is reaching the
    // sensor at all, and whether TX/RX need swapping.
    //   0x00 = no response     → UART not reaching sensor (swap TX/RX or check power)
    //   0xAA / 0x55 garbage    → baud rate mismatch
    //   0x02                   → FINGERPRINT_NOFINGER  (sensor alive, no finger)
    //   0x00 with timeout      → TX→RX wired backwards
    // ----------------------------------------------------------------
    uint32_t fingerBauds[] = {57600, 9600};
    for (uint8_t i = 0; i < 2 && !fingerOK; i++) {
        mySerial.end();
        delay(20);
        mySerial.begin(fingerBauds[i], SERIAL_8N1, 16, 17);
        finger.begin(fingerBauds[i]);
        delay(100);

        Serial.printf("[FINGER] Trying %u baud — verifyPassword()...\n", fingerBauds[i]);
        if (finger.verifyPassword()) {
            fingerOK = true;
            Serial.printf("[OK] Fingerprint sensor ready at %u baud. Templates: %d\n",
                          fingerBauds[i], finger.templateCount);
        } else {
            // Raw probe: call getImage() and print the raw return code
            uint8_t raw = finger.getImage();
            Serial.printf("[FINGER] verifyPassword failed. Raw getImage()=0x%02X  ", raw);
            if      (raw == 0x02) Serial.println("→ sensor alive but password wrong (unlikely)");
            else if (raw == 0x01) Serial.println("→ sensor responded OK");
            else if (raw == 0x00) Serial.println("→ NO RESPONSE — check VCC=3.3V and TX/RX swap");
            else                  Serial.printf ("→ unexpected code (baud mismatch or noise)\n");
        }
    }
    if (!fingerOK) {
        Serial.println("[WARN] Fingerprint sensor not responding at 57600 or 9600.");
        Serial.println("       Wiring guide for AS608/R307/R503:");
        Serial.println("         Sensor TX (yellow/white) → ESP32 GPIO16");
        Serial.println("         Sensor RX (white/yellow) → ESP32 GPIO17");
        Serial.println("         Sensor VCC               → ESP32 3.3V  (NOT 5V)");
        Serial.println("         Sensor GND               → ESP32 GND");
        Serial.println("       If still failing, swap the TX and RX wires and reboot.");
        Serial.println("       Fingerprint scanning disabled until sensor responds.");
    }

    // Servos
    // FIX: Stagger attach() calls to avoid simultaneous inrush current.
    // Attaching both servos back-to-back while the radio is also active
    // creates a current spike that triggers the brownout detector.
    main_gate.attach(SERVO_PIN_1);
    main_gate.write(0);
    delay(300);
    main_door.attach(SERVO_PIN_2);
    main_door.write(0);
    delay(300);
    Serial.println("[OK] Servos initialised at 0 degrees.");

    memset(&myData, 0, sizeof(myData));
    strncpy(myData.name, "NODE_2", sizeof(myData.name) - 1);

    Serial.println("[OK] NODE_2 ready.\n");
    Serial.println("--- Waiting for RFID card or fingerprint ---");
}

/* ================================================================
   LOOP
   ================================================================ */
void loop() {
    // 1. Servos
    tickServo(gateState);
    tickServo(doorState);

    // 2. Remote commands from NODE_1
    // FIX: Read and clear the flag inside the critical section to prevent
    // a race where the ISR sets remoteCommandReady between the check and
    // the copy, causing a command to be processed twice or missed.
    {
        bool gotCmd = false;
        struct_message cmd;
        taskENTER_CRITICAL(&cmdMux);
        if (remoteCommandReady) {
            memcpy(&cmd, &remoteCmd, sizeof(cmd));
            remoteCommandReady = false;
            gotCmd = true;
        }
        taskEXIT_CRITICAL(&cmdMux);

        if (gotCmd && strncmp(cmd.name, "NODE_1", 6) == 0) {
            /* Gate servo (LOC_GATE) */
            if (cmd.location == LOC_GATE) {
                if (cmd.command == CMD_DOOR_OPEN) {
                    Serial.printf("[REMOTE] Open GATE for user %d\n", cmd.userID);
                    openServo(gateState);
                } else if (cmd.command == CMD_DOOR_CLOSE) {
                    Serial.println("[REMOTE] Close GATE");
                    closeServo(gateState);
                } else if (cmd.command == CMD_IDLE && cmd.userID > 0) {
                    /* Legacy: face-verify granted → open gate */
                    Serial.printf("[REMOTE] Face OK → open GATE user %d\n", cmd.userID);
                    openServo(gateState);
                }
            }
            /* Main door servo (LOC_MAIN_DOOR) */
            if (cmd.location == LOC_MAIN_DOOR) {
                if (cmd.command == CMD_DOOR_OPEN) {
                    Serial.printf("[REMOTE] Open MAIN DOOR for user %d\n", cmd.userID);
                    openServo(doorState);
                } else if (cmd.command == CMD_DOOR_CLOSE) {
                    Serial.println("[REMOTE] Close MAIN DOOR");
                    closeServo(doorState);
                } else if (cmd.command == CMD_IDLE && cmd.userID > 0) {
                    /* Legacy: face-verify granted → open door */
                    Serial.printf("[REMOTE] Face OK → open DOOR user %d\n", cmd.userID);
                    openServo(doorState);
                }
            }
        }
    }

    // 3. RFID scanner
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {

        // Print the raw UID for every scan — helps confirm card is being read
        Serial.print("[RFID] Card detected. UID: ");
        for (byte i = 0; i < mfrc522.uid.size; i++) {
            Serial.printf("%02X ", mfrc522.uid.uidByte[i]);
        }
        Serial.println();

        myData.userID   = 0;
        myData.command  = 0;
        myData.location = 0;

        if      (memcmp(mfrc522.uid.uidByte, CARD_CALEB,    4) == 0) myData.userID = 1;
        else if (memcmp(mfrc522.uid.uidByte, CARD_EMMANUEL, 4) == 0) myData.userID = 2;
        else if (memcmp(mfrc522.uid.uidByte, CARD_MARY,     4) == 0) myData.userID = 3;
        else if (memcmp(mfrc522.uid.uidByte, CARD_PRINCESS, 4) == 0) myData.userID = 4;

        if (myData.userID > 0) {
            Serial.printf("[RFID] Matched: User %d\n", myData.userID);
            myData.location = LOC_GATE;
            handleAccess();
        } else {
            Serial.println("[RFID] No match — unknown card.");
            static unsigned long lastIntruderAlert = 0;
            if (millis() - lastIntruderAlert > INTRUDER_DEBOUNCE_MS) {
                myData.location  = LOC_INTRUDER;
                myData.userID    = 0;
                myData.command   = CMD_IDLE;
                myData.timestamp = millis();
                Serial.println("[ALERT] Unknown RFID at GATE — INTRUDER!");
                bool ok = masterNode->sendMessage((uint8_t *)&myData, sizeof(myData));
                if (ok) {
                    lastIntruderAlert = millis();
                } else {
                    Serial.println("[WARN] Intruder alert send failed.");
                }
            }
        }

        mfrc522.PICC_HaltA();
        mfrc522.PCD_StopCrypto1();
    }

    // 4. Fingerprint scanner
    static unsigned long lastFingerPoll  = 0;
    static unsigned long lastFingerError = 0;
    if (fingerOK && millis() - lastFingerPoll >= FINGER_POLL_MS) {
        lastFingerPoll = millis();
        int fid = getFingerprintID();
        if (fid > 0) {
            Serial.printf("[FINGER] Match: template ID %d\n", fid);
            myData.userID    = fid;
            myData.location  = LOC_MAIN_DOOR;
            myData.command   = CMD_IDLE;
            myData.timestamp = millis();
            handleAccess();
        } else if (fid < 0) {
            // Rate-limit to once per 5 s so the log stays readable
            if (millis() - lastFingerError >= 5000UL) {
                lastFingerError = millis();
                Serial.println("[FINGER] Sensor read error — check wiring on pins 16/17.");
            }
        }
    }

    // 5. Heartbeat
    static unsigned long lastHB = 0;
    if (millis() - lastHB >= HEARTBEAT_INTERVAL_MS) {
        myData.userID    = 0;
        myData.location  = LOC_HEARTBEAT;
        myData.command   = CMD_IDLE;
        myData.timestamp = millis();
        bool ok = masterNode->sendMessage((uint8_t *)&myData, sizeof(myData));
        // FIX: always update lastHB regardless of send result.
        // Previously, a failed send left lastHB unchanged, causing the
        // node to hammer the radio every loop iteration until success.
        lastHB = millis();
        if (ok) {
            Serial.println("[HB] Heartbeat sent to NODE_1.");
        } else {
            Serial.println("[WARN] Heartbeat send failed — will retry in 10s.");
        }
    }
}
