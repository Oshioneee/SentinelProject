# 📷 Node 4: Vision & On-Demand Camera Streaming Node

The **Vision Node (Node 4)** is an active surveillance node built on the **AI-Thinker ESP32-CAM** module. It operates as a dual-mode camera: running continuous low-memory grayscale motion detection by default, and switching on-demand to high-quality JPEG streaming when requested. It hosts a local HTTP server serving MJPEG live video feeds and snapshots, coordinated alongside a PWM-controlled high-power flash LED.

---

## 🔌 Hardware Specs & Pinout

This node utilizes the standard **AI-Thinker ESP32-CAM** board carrying an **OV2640** camera sensor.

| Camera Pin | ESP32 GPIO | Role / Description |
|---|---|---|
| **PWDN** | **GPIO 32** | Camera Power-Down (Active HIGH) |
| **RESET** | **GPIO -1** | Camera Hardware Reset (Disabled) |
| **XCLK** | **GPIO 0** | System Master Clock (Driven by LEDC Timer 0) |
| **SIOD** | **GPIO 26** | I2C Data (SCCB configuration line) |
| **SIOC** | **GPIO 27** | I2C Clock (SCCB configuration line) |
| **VSYNC** | **GPIO 25** | Vertical Sync (Frame Boundary) |
| **HREF** | **GPIO 23** | Horizontal Reference (Line Boundary) |
| **PCLK** | **GPIO 22** | Pixel Clock (Data Latch) |
| **D0 - D7** | **GPIO 5, 18, 19, 21, 36, 39, 34, 35** | 8-bit parallel digital camera data bus |
| **FLASH LED** | **GPIO 4** | Onboard high-power white LED (Driven by LEDC Timer 1) |
| **VCC** | **5V** | Must be powered from stable 5V rail (Wi-Fi + Flash uses >600mA) |
| **GND** | **GND** | System common ground |

---

## 🌓 Dual Camera Operation Modes

To maximize PSRAM lifetime and prevent memory corruption, the node operates in two separate software modes:

```
[Detection Mode] (Default)
 ├── Grayscale QVGA Format (320x240)
 ├── Persistent PSRAM buffer differencing
 ├── HTTP Server: OFF
 └── ESP-NOW Motion alerts active
       │
       ▼ (Hub sends CMD_START_STREAM)
[Streaming Mode]
 ├── JPEG QVGA Format
 ├── HTTP Server: ON (Port 80)
 ├── GET /        -> 30 FPS MJPEG video stream
 ├── GET /capture -> Cache-busted snapshot
 └── ESP-NOW heartbeats active
```

### 1. Grayscale Motion Detection (Default)
* **Configuration**: Set to `PIXFORMAT_GRAYSCALE` and resolution `FRAMESIZE_QVGA` (320×240 $\rightarrow$ 76,800 pixels). 
* **Buffer Allocation**: Allocates a single persistent PSRAM buffer (`prevFrame`) on the first loop cycle. By reusing this buffer, it completely avoids calling `ps_malloc` / `free` in the loop, **eliminating heap fragmentation**.
* **Algorithm**: Performs pixel-by-pixel frame differencing:
  $$\text{Absolute Difference} = | \text{Current Pixel} - \text{Previous Pixel} |$$
  * If the absolute difference is $>30$ on more than **5% of the total frame pixels** (approx. 3,840 pixels), motion is triggered.
  * Sends an ESP-NOW alert (`location = 3`) to the Hub. Enforces a **5-second send cooldown** to prevent packet flooding.

### 2. JPEG Streaming Mode (On-Demand)
* **Transition**: Triggered by receiving `CMD_START_STREAM` (1). The node frees the grayscale buffer, deinitializes the camera, toggles the camera PWDN pin to hard power-cycle the sensor, reinitializes in `PIXFORMAT_JPEG`, and starts the HTTP Server on port 80.
* **MJPEG Live Stream (`GET /`)**: Loops while `isStreaming = true`, grabbing JPEG frames and pushing them separated by boundary headers. Includes a `vTaskDelay(33ms)` yield on every frame to let the Wi-Fi transmission buffers drain, **preventing video stuttering or socket lockups**.
* **Snapshot Fetch (`GET /capture`)**: Serves a single JPEG frame to the Hub for Face++ verification. The response utilizes custom headers to disable caching:
  ```http
  Cache-Control: no-cache, no-store, must-revalidate
  Pragma: no-cache
  Expires: 0
  ```
  This prevents browsers and Blynk from caching the frame, **fixing a bug where the dashboard webcam feed froze on the first captured frame**.

---

## 🎛️ PWM Flash LED Control

The onboard flash LED is driven using the ESP32's **LEDC PWM** peripheral:
* **Speed Mode**: `LEDC_LOW_SPEED_MODE`
* **Timer Selection**: `LEDC_TIMER_1` (Timer 0 is dedicated to the camera XCLK)
* **Channel Selection**: `LEDC_CHANNEL_1`
* **Frequency**: `5000Hz`
* **Resolution**: 8-bit (`0 - 255` duty cycle)

### Auto-Off with Preference Restoration
* When the user adjusts the dashboard slider, `CMD_SET_FLASH` updates `userFlashLevel` (storing their preference).
* During stream transitions, the flash remains active. 
* Once streaming terminates, a **3-second auto-off timer** is scheduled. After 3 seconds, the LED is automatically restored to the user's manual slider level rather than turning off completely, **preventing manual preference loss**.

---

## 🛠️ Connectivity Self-Healing Features

ESP32-CAM boards are notorious for wireless dropped packets. Node 4 incorporates robust self-healing layers to ensure continuous operation:

1. **Active Wi-Fi Reconnect Loop**:
   * The main loop continuously checks `WiFi.status()`. If it drops, it non-blockingly attempts to reconnect to `SENTINEL-HUB` using static IP configuration (`192.168.4.100`), ensuring the Hub can always reach the `/capture` URL.
2. **Post-Reconnect Channel Recovery**:
   * *Problem*: When Wi-Fi reconnects, the radio can jump to a different channel, causing ESP-NOW packets to fail with channel mismatch errors.
   * *Solution*: After a successful Wi-Fi reconnection, the node explicitly re-asserts its channel:
     ```cpp
     WiFi.setChannel(6);
     ```
3. **Camera PWDN Hard Reset**:
   * *Problem*: Repeated camera deinitialization and reinitialization cycles leave the OV2640's internal SCCB (I2C) register map corrupted, triggering `0x106` (Detected camera not supported) boot errors.
   * *Solution*: The power-cycle pin `PWDN (GPIO 32)` is driven `HIGH` (sensor off) for 20ms, then driven `LOW` (sensor on) for 20ms before calling `esp_camera_init()`, ensuring a clean boot.
4. **GPIO ISR Pre-Installation**:
   * Setup calls `gpio_install_isr_service(0)` once before camera init, preventing the camera driver from throwing "GPIO isr service already installed" error loops.
