# 🛡️ Node 3: Environmental Safety & Gas Interlock Node

The **Environmental Safety Node (Node 3)** monitors local air quality for hazardous gas leaks and tracks human presence across four distinct physical zones. It possesses autonomous physical override capabilities to instantly shut off gas lines, sound alarms, turn on ventilation fans, and actuate emergency egress backdoor servos.

---

## 🔌 Hardware Wiring & Pinout

This node is built on an **ESP32 development module**.

| Peripheral | Component Pin | ESP32 GPIO | Description / Notes |
|---|---|---|---|
| **MQ-6 Gas Sensor** | Analog Out (AO) | **GPIO 34** | ADC1 Channel 6 (LPG and Methane) |
| **MQ-2 Gas Sensor** | Analog Out (AO) | **GPIO 35** | ADC1 Channel 7 (Smoke and Hydrogen) |
| **PIR Zone 1** | Out | **GPIO 13** | HC-SR501 active-HIGH motion sensor |
| **PIR Zone 2** | Out | **GPIO 26** | HC-SR501 active-HIGH motion sensor |
| **PIR Zone 3** | Out | **GPIO 14** | HC-SR501 active-HIGH motion sensor |
| **PIR Zone 4** | Out | **GPIO 25** | HC-SR501 active-HIGH motion sensor |
| **Solenoid Valve** | Relay Signal | **GPIO 32** | Active-LOW relay (HIGH = Open, LOW = Shutoff) |
| **Ventilation Fan** | Relay Signal | **GPIO 33** | Active-HIGH relay (HIGH = Fan ON, LOW = Fan OFF) |
| **Backdoor Servo** | PWM (Orange) | **GPIO 18** | Emergency backdoor egress servo |
| **Buzzer** | VCC | **GPIO 19** | 5V Active Piezo Buzzer (900Hz alarm tone) |
| **All Sensors** | VCC | **5V / 3.3V** | Gas heaters pull ~150mA $\rightarrow$ connect to stable 5V |
| **Unified Ground**| GND | **GND** | Direct ground link across all devices |

---

## 🧪 Gas Sensor Calibration & Curve Mathematics

Gas sensors use chemical heating resistors. On boot, they require a **45-second preheating** period before R0 calibration is initiated to prevent infinite or invalid concentration readings (`nan` or `inf`).

### 1. Clean Air Calibration ($R_0$)
Calibration takes place in clean air. The sensor resistance in clean air is divided by a datasheet-defined clean air ratio constant:
* **MQ-6 ratio in clean air**: $10.0$
* **MQ-2 ratio in clean air**: $9.83$

Firmware averages 15 calibration runs over 7.5 seconds using:
```cpp
sensor.calibrate(ratioInCleanAir);
```

### 2. Concentration Calculation (PPM)
Gas concentrations are computed using the exponential power-law model:
$$\text{ppm} = a \times \left(\frac{R_s}{R_0}\right)^b$$

The regression constants ($a$ and $b$) are loaded from the MQ Unified Sensor curves:
* **LPG (MQ-6)**: $a = 1009.2$, $b = -2.35$
* **CH4 Methane (MQ-6)**: $a = 2127.2$, $b = -2.526$
* **Smoke (MQ-2)**: $a = 36974.0$, $b = -3.109$
* **H2 Hydrogen (MQ-2)**: $a = 987.99$, $b = -2.162$

---

## 🛠️ Critical Firmware Safety Enhancements

1. **Floating Pin Suppression**:
   * *Problem*: Unconnected or loosely wired PIR pins float, picking up electromagnetic noise and triggering random high values.
   * *Solution*: Pins are initialized with internal pull-downs:
     ```cpp
     pinMode(PIR_PINS[i], INPUT_PULLDOWN);
     ```
2. **PIR Clear Debounce Latching**:
   * *Problem*: A bouncing PIR pin returning to `LOW` for even a single microsecond cleared the global PIR tracking state. The subsequent blip back to `HIGH` was detected as a brand new trigger, bypassing the resend cool-down timer and flooding the ESP-NOW network with thousands of packets.
   * *Solution*: The system introduces a strict **2-second clear debounce** (`PIR_CLEAR_DEBOUNCE_MS = 2000`). PIR signals must remain continuously low for 2 seconds before the "all clear" packet is sent and the state resets, filtering out transient spikes.
3. **Independent Alarm State Tracking**:
   * *Problem*: Gas and PIR alerts both used the telemetry coordinates `location = 3`. If gas recovered while PIR was active, the gas logic sent a "clear" packet that mistakenly reset the intruder indicators as well.
   * *Solution*: Global states are strictly decoupled in the loop. The `gasDangerActive` boolean tracks gas hazard status independently from `pirActive` motion status.
4. **Safety Ventilation Latching**:
   * *Problem*: If gas was successfully vented, shutting down the ventilation fan immediately could leave toxic pockets of gas in areas with occupants.
   * *Solution*: We implement an environmental safety lock: if gas returns to normal, the system only closes the backdoor and turns the fan OFF **if no motion is currently detected** (`!pirActive`) inside the room. If motion exists, the fan is kept running until the room is empty.

---

## 🚨 Emergency Safety Protocol

When any sensor reading crosses its safety threshold:
* **Thresholds**: LPG $>1000\text{ppm}$ \| CH4 $>1000\text{ppm}$ \| Smoke $>300\text{ppm}$ \| H2 $>500\text{ppm}$

The node **autonomously executes the safety interlock sequence** in less than 100ms:
1. Shuts off the Solenoid Gas Valve relay (failsafe LOW).
2. Activates the Ventilation Fan relay (HIGH).
3. Sweeps the Emergency Egress backdoor servo to **90°** (Open).
4. Sounds the Piezo Buzzer at **900Hz**.
5. Transmits a high-priority gas danger alert packet to the Hub (`location = 3`).

---

## 📨 ESP-NOW Blynk Override API

The Hub can manually override environmental safety relays (e.g. from the Blynk dashboard):
* **`CMD_VENT_ON` / `CMD_VENT_OFF`**: Explicitly override fan state (off is blocked if gas is still dangerous).
* **`CMD_VALVE_ON` / `CMD_VALVE_OFF`**: Manually open/close the solenoid gas valve.
* **`CMD_DOOR_OPEN` / `CMD_DOOR_CLOSE`**: Override emergency backdoor servo lock angle.
