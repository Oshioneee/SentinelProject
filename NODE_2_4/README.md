# 🔒 Node 2: Security & Physical Access Ingress Node

The **Security Node (Node 2)** handles physical gate and front-door entry access control. It combines high-frequency RFID token scanning with high-precision biometric fingerprint validation to unlock physical door lock servos. It operates in close collaboration with the Hub (Node 1) for remote logging, facial recognition override gates, and system-wide intruder alert telemetry.

---

## 🔌 Hardware Wiring & Pinout

This node is built on a standard **ESP32 development module** (NodeMCU or ESP32-WROOM-32D). 

| Peripheral | Sensor Pin | ESP32 GPIO | Description / Notes |
|---|---|---|---|
| **MFRC522 RFID** | SCK | **GPIO 18** | SPI Clock |
| **MFRC522 RFID** | MISO | **GPIO 19** | SPI Master In Slave Out |
| **MFRC522 RFID** | MOSI | **GPIO 23** | SPI Master Out Slave In |
| **MFRC522 RFID** | SDA (SS) | **GPIO 5** | SPI Chip Select (Software Pull-up active) |
| **MFRC522 RFID** | RST | **GPIO 4** | Hard Reset line (pulsed in setup) |
| **MFRC522 RFID** | VCC | **3.3V** | Do NOT connect to 5V (damages chip) |
| **AS608/R307** | TX (Yellow) | **GPIO 16** | UART2 RX2 (Pulls data from sensor) |
| **AS608/R307** | RX (White) | **GPIO 17** | UART2 TX2 (Sends commands to sensor) |
| **AS608/R307** | VCC (Red) | **3.3V** | Dual-baud power (57600 or 9600) |
| **SG90 Servo 1** | PWM (Orange) | **GPIO 13** | Main Gate servo control |
| **SG90 Servo 2** | PWM (Orange) | **GPIO 27** | Main Entry Door servo control |
| **Both Servos** | VCC (Red) | **5V / External**| High inrush load — power from external source |
| **All Devices** | GND (Black) | **GND** | Unified system ground reference |

---

## 🛠️ Critical Embedded Bug Fixes & Optimizations

This node implements advanced physical and firmware safety patterns to solve common ESP32 hardware bugs:

1. **Servo-Driven Brownout Prevention**:
   * *Problem*: Actuating two servo motors simultaneously alongside the active ESP-NOW radio transceiver can pull transient current spikes exceeding **1.0A**, drooping the 3.3V rail below the ESP32's built-in 2.45V threshold. This previously led to infinite reboot loops.
   * *Solution*: 
     * Inside `setup()`, the brownout detector is disabled using:
       ```cpp
       WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
       ```
     * Servo `attach()` sequences are staggered by a `300ms` non-blocking delay to spread out inductive inrush currents.
2. **SS Strapping Pin Safe Start**:
   * *Problem*: **GPIO 5** is a native ESP32 strapping pin. If left floating or pulled low by the MFRC522 during boot, it forces the chip into an incorrect boot loader state.
   * *Solution*: We explicitly deassert the Chip Select line by setting GPIO 5 as an output and driving it `HIGH` *before* initiating SPI buses:
     ```cpp
     pinMode(SS_PIN, OUTPUT);
     digitalWrite(SS_PIN, HIGH);
     ```
3. **MFRC522 SPI Lockup Recovery**:
   * *Problem*: The standard SPI library can leave the MFRC522 in a half-configured state on startup, yielding default register readings of `0x00` or `0xFF` (indicating bus failure).
   * *Solution*: Setup hard-pulses the RFID **RST pin (GPIO 4)** low for 10ms, then waits 50ms (the datasheet requires $\ge 37$ms) for the internal oscillators to stabilize before invoking `PCD_Init()`.
4. **Heartbeat Rate Limit Loop Fix**:
   * *Problem*: If an ESP-NOW transmission failed, the heartbeat task failed to update its timer variable (`lastHB`), causing the node to hammer the ESP-NOW radio on every single loop cycle, leading to buffer saturation.
   * *Solution*: `lastHB` is now updated *unconditionally* upon transmission attempt, enforcing a strict 10-second interval regardless of the network state.

---

## 🔑 Access Ingress Rules & Credentials

### Authorized RFID Cards
Scanning any of the following 4-byte UIDs automatically opens the **Main Gate Servo (GPIO 13)** to 90°, holds it open for 5 seconds, drives it back to 0°, and reports the user's name and ID to the Hub:

* **Caleb** (User ID 1): `33 C0 EE 05`
* **Emmanuel** (User ID 2): `AE 60 6A 06`
* **Mary** (User ID 3): `BF EA 58 86`
* **Princess** (User ID 4): `51 CA 45 6A`

### Biometric Ingress
Placing an enrolled finger on the AS608 reader matches it against local templates. A successful fingerprint match automatically opens the **Main Door Servo (GPIO 27)** to 90°, holds it for 5 seconds, and logs the entrance at Node 1.

### 🚨 Intruder Protocol
* If an unregistered RFID token is scanned, Node 2 immediately transmits a custom packet with `location = 3` (LOC_INTRUDER), `userID = 0`, and `command = CMD_IDLE`. 
* Node 1 receives this, plays an alarm tone, flashes red warning lights, and publishes an **"Unknown RFID - INTRUDER!"** alert to the Blynk interface.

---

## 📨 ESP-NOW Inbound API Handlers

Node 2 registers an ISR receive callback that monitors inbound instructions from the Sentinel Hub (Node 1):

* **`CMD_DOOR_OPEN`**:
  * If `location == LOC_GATE` (1) $\rightarrow$ Actuates gate servo to 90° for 5 seconds.
  * If `location == LOC_MAIN_DOOR` (2) $\rightarrow$ Actuates door servo to 90° for 5 seconds.
* **`CMD_DOOR_CLOSE`**:
  * Override to immediately close the gate or door servos (canceling the 5-second automatic delay timer).
