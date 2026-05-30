/*
 * =============================================================
 *  NODE_4.ino  —  Vision Node  (AI Thinker ESP32-CAM, Core 3.x)
 * =============================================================
 *  Hardware:
 *    OV2640 camera (via camera_pin.h AI_THINKER pinout)
 *
 *  MAC addresses:
 *    This node  (STA): 68:25:DD:2D:D7:1C
 *    NODE_1 (master) : 90:70:69:16:9A:FC
 *
 *  ESP-NOW roles:
 *    SENDS  → NODE_1 : motion alerts, heartbeats
 *    RECEIVES ← NODE_1 : command=1 (start stream/JPEG mode)
 *                        command=2 (stop stream, return to detection)
 *
 *  HTTP endpoints (active only while isStreaming=true):
 *    GET /         → MJPEG stream  (~30 fps)
 *    GET /capture  → Single JPEG snapshot (used by NODE_1 for face verify)
 *
 *  Motion detection:
 *    Uses frame differencing on GRAYSCALE QVGA (320×240).
 *    A single persistent PSRAM buffer stores the previous frame.
 *    Avoids repeated alloc/free which fragments PSRAM over time.
 *
 *  Face verification:
 *    NODE_4 does NOT do on-device face recognition (the legacy
 *    fd_forward.h/fr_forward.h API was removed in Core 3.x).
 *    NODE_1 fetches the JPEG via /capture and calls the
 *    Face++ cloud API for identification.
 *
 *  IMPORTANT:
 *    ESPNOW_WIFI_CHANNEL must match your router and all other nodes.
 *    The AI Thinker board uses GPIO4 as the flash LED — do not use
 *    GPIO4 for anything else.
 * =============================================================
 */

#define CAMERA_MODEL_AI_THINKER   // Selects correct pins in camera_pin.h
#include "camera_pin.h"
#include <Arduino.h>
#include "ESP32_NOW.h"            // Arduino wrapper — for sending to NODE_1
#include "WiFi.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_now.h"              // IDF low-level — for receive callback
#include "driver/gpio.h"          // gpio_install_isr_service — called once in setup()

/* ================================================================
   CONFIGURATION
   ================================================================ */
#define ESPNOW_WIFI_CHANNEL        6
#define MOTION_CHECK_INTERVAL_MS   300UL
#define MOTION_SEND_COOLDOWN_MS    5000UL
#define MOTION_THRESHOLD           30
#define MOTION_PIXEL_PERCENT       5
#define STREAM_FRAME_DELAY_MS      33UL
#define CAMERA_REINIT_DELAY_MS     150UL
#define HEARTBEAT_INTERVAL_MS      10000UL

/* Flash LED — AI Thinker GPIO4, controlled via LEDC PWM for brightness */
#define FLASH_LED_PIN      LED_GPIO_NUM   // = 4 on AI Thinker (from camera_pin.h)
#define FLASH_LEDC_CH      LEDC_CHANNEL_1 // Channel 0 used by camera XCLK
#define FLASH_LEDC_TIMER   LEDC_TIMER_1
#define FLASH_FREQ_HZ      5000
#define FLASH_RESOLUTION   LEDC_TIMER_8_BIT  // 0-255 duty

/* Command constants (must match config.h and shared_types.h) */
#define CMD_IDLE          0
#define CMD_START_STREAM  1
#define CMD_STOP_STREAM   2
#define CMD_SET_FLASH     9   /* brightness in userID field, 0=off 255=max */

/* ================================================================
   MAC ADDRESSES
   ================================================================ */
static uint8_t master_mac[] = {0x90, 0x70, 0x69, 0x16, 0x9A, 0xFC};

/* ================================================================
   SHARED PACKET STRUCTURE  (identical on all four nodes)
   ================================================================ */
typedef struct __attribute__((packed)) {
    char          name[10];
    int           userID;     // 0 = motion/unknown  1-4 = named person (set by NODE_1)
    int           location;   // 0=heartbeat  2=main door (this node)  3=motion detected
    float         lpg;        // Unused on NODE_4 — always 0
    float         CH4;
    float         smoke;
    float         hydrogen;
    int           command;    // 0=idle  1=start_stream  2=stop_stream
    unsigned long timestamp;
} struct_message;

static struct_message myData;     // Outgoing packets
static struct_message incoming;   // Incoming command from NODE_1

/* ================================================================
   STATE FLAGS
   isStreaming: written from WiFi task (recv callback), read from loop().
   Marked volatile to prevent the compiler from caching it in a register.
   ================================================================ */
static volatile bool   isStreaming    = false;
static httpd_handle_t  stream_httpd   = NULL;

// Previous grayscale frame for motion differencing
// Allocated once in PSRAM on first detectMotion() call; never freed.
static uint8_t *prevFrame      = nullptr;
static bool     prevFrameValid = false;

/* ================================================================
   ESP-NOW PEER CLASS  (used for sending to NODE_1 only)
   ================================================================ */
class MasterPeer : public ESP_NOW_Peer {
public:
    MasterPeer(const uint8_t *mac, uint8_t ch)
        : ESP_NOW_Peer(mac, ch, WIFI_IF_STA, nullptr) {}
    bool init()                                     { return add(); }
    bool sendMessage(const uint8_t *d, size_t len) { return send(d, len); }
};

static MasterPeer *masterNode = nullptr;

/* Flash brightness — written by CMD_SET_FLASH handler */
static uint8_t  userFlashLevel  = 0;   /* Slider-set preference — survives captures */
static uint8_t  flashBrightness = 0;   /* Current PWM level    — may differ during capture */
static bool     flashAutoOff    = false;
static unsigned long flashOffMs = 0;

/* ================================================================
   FLASH LED CONTROL  (LEDC PWM on GPIO4)
   ================================================================ */
static void initFlashLED() {
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = FLASH_RESOLUTION,
        .timer_num       = FLASH_LEDC_TIMER,
        .freq_hz         = FLASH_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num   = FLASH_LED_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = FLASH_LEDC_CH,
        .timer_sel  = FLASH_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);
    Serial.println("[OK] Flash LED PWM initialised on GPIO4.");
}

static void setFlash(uint8_t brightness) {
    flashBrightness = brightness;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, FLASH_LEDC_CH, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, FLASH_LEDC_CH);
    Serial.printf("[FLASH] Brightness → %d\n", brightness);
}

/* ================================================================
   RECEIVE CALLBACK  (Core 3.x IDF low-level API)
   MUST be fast — Serial.printf REMOVED (unsafe in IRAM context).
   ================================================================ */
static volatile bool   newCommand   = false;
static volatile int    latestCmd    = 0;
static volatile int    latestCmdVal = 0;   // userID field (used for flash brightness)

static void IRAM_ATTR onDataReceived(const esp_now_recv_info_t *info,
                                      const uint8_t            *data,
                                      int                       len) {
    if (!info || !data || len < (int)sizeof(struct_message)) return;
    if (memcmp(info->src_addr, master_mac, 6) != 0) return;

    memcpy(&incoming, data, sizeof(struct_message));
    latestCmd    = incoming.command;
    latestCmdVal = incoming.userID;
    newCommand   = true;

    /* Only update isStreaming directly for speed-critical stream commands */
    if      (incoming.command == CMD_START_STREAM) isStreaming = true;
    else if (incoming.command == CMD_STOP_STREAM)  isStreaming = false;
}

/* ================================================================
   HTTP HANDLERS
   ================================================================ */

/*
 * MJPEG stream handler — GET /
 * Loops while isStreaming, sending JPEG frames separated by
 * multipart boundaries. vTaskDelay yields to the WiFi TX buffer
 * preventing saturation (without it the stream stalls/crashes).
 */
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb  = NULL;
    esp_err_t    res = ESP_OK;
    char         part_buf[64];  // Plain char array (not char *[64] — that was a prior UB bug)

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    while (isStreaming) {
        fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("[WARN] stream: fb_get failed — aborting.");
            res = ESP_FAIL;
            break;
        }

        size_t hlen = snprintf(part_buf, sizeof(part_buf),
            "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
            (unsigned)fb->len);

        res = httpd_resp_send_chunk(req, part_buf, (ssize_t)hlen);
        if (res == ESP_OK)
            res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        if (res == ESP_OK)
            res = httpd_resp_send_chunk(req, "\r\n--frame\r\n", 11);

        esp_camera_fb_return(fb);
        if (res != ESP_OK) break;

        // ~30 fps cap and WiFi TX buffer yield
        vTaskDelay(pdMS_TO_TICKS(STREAM_FRAME_DELAY_MS));
    }

    httpd_resp_send_chunk(req, NULL, 0);  // End chunked transfer cleanly
    return res;
}

/*
 * Single JPEG snapshot — GET /capture
 * NODE_1 calls this after sending command=1 to grab one frame
 * for Face++ verification and Imgbb upload.
 * Returns 503 if camera is still in grayscale detection mode
 * (NODE_1 should retry after the 2s startup delay).
 */
static esp_err_t capture_handler(httpd_req_t *req) {
    // Capture is only meaningful when in JPEG mode (streaming mode)
    // If still in grayscale, return 503 so NODE_1 knows to retry
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (fb->format != PIXFORMAT_JPEG) {
        esp_camera_fb_return(fb);
        httpd_resp_set_status(req, "503 Camera Not Ready");
        httpd_resp_sendstr(req, "Camera not in JPEG mode yet. Retry in 1s.");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    // FIX: Prevent browser/dashboard caching the capture image.
    // Without these headers the dashboard img tag reuses the first fetched
    // frame on every subsequent /capture request — image appears frozen.
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);

    Serial.printf("[HTTP] Snapshot served: %u bytes\n", (unsigned)fb->len);
    return res;
}

/* ================================================================
   HTTP SERVER LIFECYCLE
   ================================================================ */
static void start_stream_server() {
    if (stream_httpd) return;  // Already running

    httpd_config_t config     = HTTPD_DEFAULT_CONFIG();
    config.server_port        = 80;
    config.ctrl_port          = 32768;
    config.max_uri_handlers   = 4;    // room for stream + capture + future endpoints
    config.max_resp_headers   = 8;    // default is 4 — increased for no-cache headers on /capture

    httpd_uri_t stream_uri = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = stream_handler,
        .user_ctx = NULL
    };
    httpd_uri_t capture_uri = {
        .uri      = "/capture",
        .method   = HTTP_GET,
        .handler  = capture_handler,
        .user_ctx = NULL
    };

    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
        httpd_register_uri_handler(stream_httpd, &capture_uri);
        Serial.printf("[OK] HTTP server started. Stream: http://%s/  Capture: http://%s/capture\n",
                      WiFi.localIP().toString().c_str(),
                      WiFi.localIP().toString().c_str());
    } else {
        Serial.println("[ERROR] HTTP server failed to start.");
        stream_httpd = NULL;
    }
}

static void stop_stream_server() {
    if (!stream_httpd) return;
    httpd_stop(stream_httpd);
    stream_httpd = NULL;
    Serial.println("[INFO] HTTP server stopped.");
    delay(100);  // Allow TCP sockets to fully release before potential port reuse
}

/* ================================================================
   CAMERA INITIALISATION
   Returns true on success.
   ================================================================ */
static bool setupCamera(pixformat_t format, framesize_t size) {
    // ----------------------------------------------------------------
    // FIX: Power-cycle the camera via PWDN before every init.
    // Without this, repeated deinit/init cycles corrupt the OV2640's
    // I2C state, causing "Detected camera not supported" (0x106) after
    // 5-10 stream transitions — exactly the failure seen in serial logs.
    // Toggling PWDN HIGH→LOW fully resets the sensor between inits.
    // ----------------------------------------------------------------
    if (PWDN_GPIO_NUM >= 0) {
        pinMode(PWDN_GPIO_NUM, OUTPUT);
        digitalWrite(PWDN_GPIO_NUM, HIGH);  // Assert power-down (sensor off)
        delay(20);
        digitalWrite(PWDN_GPIO_NUM, LOW);   // Release power-down (sensor on)
        delay(20);
    }

    camera_config_t config;
    config.ledc_channel  = LEDC_CHANNEL_0;
    config.ledc_timer    = LEDC_TIMER_0;
    config.pin_d0        = Y2_GPIO_NUM;
    config.pin_d1        = Y3_GPIO_NUM;
    config.pin_d2        = Y4_GPIO_NUM;
    config.pin_d3        = Y5_GPIO_NUM;
    config.pin_d4        = Y6_GPIO_NUM;
    config.pin_d5        = Y7_GPIO_NUM;
    config.pin_d6        = Y8_GPIO_NUM;
    config.pin_d7        = Y9_GPIO_NUM;
    config.pin_xclk      = XCLK_GPIO_NUM;
    config.pin_pclk      = PCLK_GPIO_NUM;
    config.pin_vsync     = VSYNC_GPIO_NUM;
    config.pin_href      = HREF_GPIO_NUM;
    config.pin_sccb_sda  = SIOD_GPIO_NUM;
    config.pin_sccb_scl  = SIOC_GPIO_NUM;
    config.pin_pwdn      = PWDN_GPIO_NUM;
    config.pin_reset     = RESET_GPIO_NUM;
    config.xclk_freq_hz  = 20000000;
    config.pixel_format  = format;
    config.frame_size    = size;
    config.jpeg_quality  = 12;       // 0=best, 63=worst — 12 is a good balance
    config.fb_count      = (format == PIXFORMAT_JPEG) ? 2 : 1;
    config.fb_location   = CAMERA_FB_IN_PSRAM;
    config.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;  // Ensures freshest frame

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[ERROR] Camera init failed: 0x%x\n", err);
        return false;
    }

    // Fine-tune image sensor settings
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_framesize(s, size);
        s->set_quality(s, 12);
        s->set_brightness(s, 1);   // Slight brightness boost for indoor use
        s->set_saturation(s, 0);
    }
    return true;
}

/* ================================================================
   MOTION DETECTION
   Compares current grayscale frame against previous frame.
   Uses a single persistent PSRAM buffer — allocated once on the
   first call and reused forever to prevent heap fragmentation.
   Returns true if enough pixels changed to indicate motion.
   ================================================================ */
static bool detectMotion() {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return false;

    bool   motion     = false;
    size_t pixelCount = fb->width * fb->height;  // 320×240 = 76,800 for QVGA

    // Allocate persistent previous-frame buffer on first call
    if (!prevFrame) {
        prevFrame = (uint8_t *)ps_malloc(pixelCount);   // Allocate from PSRAM
        if (!prevFrame) {
            Serial.println("[WARN] ps_malloc for prevFrame failed.");
            esp_camera_fb_return(fb);
            return false;
        }
        memcpy(prevFrame, fb->buf, pixelCount);
        prevFrameValid = true;
        esp_camera_fb_return(fb);
        return false;  // First frame — no previous to diff against
    }

    if (prevFrameValid) {
        uint32_t diffPixels   = 0;
        uint32_t triggerCount = (uint32_t)((pixelCount * MOTION_PIXEL_PERCENT) / 100);

        // Count pixels that changed beyond the threshold
        for (size_t i = 0; i < pixelCount; i++) {
            if (abs((int)fb->buf[i] - (int)prevFrame[i]) > MOTION_THRESHOLD)
                diffPixels++;
        }

        if (diffPixels > triggerCount) {
            motion = true;
            Serial.printf("[MOTION] %u / %u pixels changed (trigger=%u)\n",
                          diffPixels, (uint32_t)pixelCount, triggerCount);
        }
    }

    // Store current frame as next "previous"
    memcpy(prevFrame, fb->buf, pixelCount);
    esp_camera_fb_return(fb);
    return motion;
}

/* ================================================================
   SETUP
   ================================================================ */
void setup() {
    Serial.begin(115200);
    Serial.println("\n[BOOT] NODE_4 Vision Node starting...");
    Serial.printf("[INFO] This node MAC: 68:25:DD:2D:D7:1C\n");
    Serial.printf("[INFO] Master (NODE_1) MAC: 90:70:69:16:9A:FC\n");

    WiFi.mode(WIFI_STA);

    /* Connect to NODE_1's local AP so this node gets a real IP.
     * The HTTP server at /capture requires a valid IP — ESP-NOW alone
     * does not assign one. SENTINEL-HUB is always on channel 6 which
     * matches ESPNOW_WIFI_CHANNEL, so ESP-NOW is unaffected.           */
    Serial.print("[WIFI] Connecting to SENTINEL-HUB");

    /* Static IP so NODE_1 always knows where to fetch /capture.
     * Update NODE4_IP in config.h to "192.168.4.100"           */
    IPAddress staticIP(192, 168, 4, 100);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.config(staticIP, gateway, subnet);

    WiFi.begin("SENTINEL-HUB", "sentinel123");
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        Serial.print(".");
        retries++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[OK] SENTINEL-HUB connected. IP: %s\n",
                      WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n[WARN] SENTINEL-HUB not reachable — /capture URL will be 0.0.0.0.");
        Serial.println("       Ensure NODE_1 is powered and booted first.");
        /* Continue anyway — ESP-NOW still works, only HTTP capture is affected */
    }

    /* FIX: Pin WiFi channel to 6 so ESP-NOW uses the correct channel
     * regardless of AP state. NODE_2 and NODE_3 both do this explicitly;
     * NODE_4 was the only node skipping it, causing potential channel
     * mismatch when NODE_1's AP was not yet up during ESP_NOW.begin(). */
    WiFi.setChannel(ESPNOW_WIFI_CHANNEL);

    /* Flash LED — init before camera so LEDC channels don't conflict */
    initFlashLED();

    if (!ESP_NOW.begin()) {
        Serial.println("[ERROR] ESP-NOW init failed — halting.");
        while (1) delay(1000);
    }

    masterNode = new MasterPeer(master_mac, ESPNOW_WIFI_CHANNEL);
    if (!masterNode->init()) {
        Serial.println("[ERROR] Failed to add master peer — halting.");
        while (1) delay(1000);
    }

    // Register receive callback (IDF low-level — works on Core 3.x)
    // Note: ESP_NOW_Class::onReceive() was removed in Core 3.x; this is
    //       the correct replacement using the underlying IDF API.
    esp_err_t cbErr = esp_now_register_recv_cb(onDataReceived);
    if (cbErr != ESP_OK) {
        Serial.printf("[ERROR] recv_cb register failed: 0x%02X\n", cbErr);
    } else {
        Serial.println("[OK] Receive callback registered.");
    }

    /* FIX: Install GPIO ISR service once here, before camera init.
     * esp_camera_init() calls gpio_install_isr_service() internally every
     * time it runs. Pre-installing it here means all subsequent camera
     * reinit calls get a silent no-op instead of the repeated
     * "GPIO isr service already installed" error spam seen in serial logs. */
    gpio_install_isr_service(0);

    // Start in grayscale mode for motion detection
    if (!setupCamera(PIXFORMAT_GRAYSCALE, FRAMESIZE_QVGA)) {
        Serial.println("[ERROR] Camera init failed — halting.");
        while (1) delay(1000);
    }

    // Initialise outgoing packet
    memset(&myData, 0, sizeof(myData));
    strncpy(myData.name, "NODE_4", sizeof(myData.name) - 1);
    myData.location = 2;  // This node is always at location=2 (main door camera)

    Serial.printf("[OK] NODE_4 ready. Local IP (after stream starts): check Serial.\n");
}

/* ================================================================
   LOOP
   ================================================================ */
void loop() {

    /* ── WiFi reconnect guard ──────────────────────────────────────────────
     * NODE_4 connects to NODE_1's AP (SENTINEL-HUB) during setup() for a
     * static IP. If the AP was not ready at boot time, or the connection
     * dropped, the /capture URL returns 0.0.0.0 and every JPEG fetch fails.
     * This guard re-runs the connect sequence at the top of every loop so
     * the node recovers automatically without needing a reboot.            */
    if (WiFi.status() != WL_CONNECTED) {
        Serial.print("[WIFI] Reconnecting to SENTINEL-HUB");
        WiFi.begin("SENTINEL-HUB", "sentinel123");
        int r = 0;
        while (WiFi.status() != WL_CONNECTED && r++ < 20) {
            delay(500);
            Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("\n[WIFI] Reconnected. IP: %s\n",
                          WiFi.localIP().toString().c_str());
            // FIX: Restore ESP-NOW channel after reconnect.
            // WiFi reassociation can land on a different channel, breaking
            // ESP-NOW with "Peer channel is not equal to home channel".
            WiFi.setChannel(ESPNOW_WIFI_CHANNEL);
        } else {
            Serial.println("\n[WIFI] Reconnect failed — will retry next loop.");
        }
    }

    /* ── Process commands from NODE_1 (safe to handle here, not in callback) ── */
    if (newCommand) {
        int cmd = latestCmd;
        int val = latestCmdVal;
        newCommand = false;

        Serial.printf("[RX] command=%d val=%d isStreaming=%d\n",
                      cmd, val, (int)isStreaming);

        if (cmd == CMD_SET_FLASH) {
            int bri = val;
            if (bri < 0)   bri = 0;
            if (bri > 255) bri = 255;
            userFlashLevel = (uint8_t)bri;   /* Remember user's preference */
            setFlash((uint8_t)bri);
        }
    }

    /* Flash auto-off: 3 s after stream stops, restore to user's slider level.
     * Previously this called setFlash(0) which wiped the slider setting.   */
    if (flashAutoOff && millis() - flashOffMs >= 3000UL) {
        flashAutoOff = false;
        setFlash(userFlashLevel);
        Serial.printf("[FLASH] Restored to user level %d after stream.\n", userFlashLevel);
    }

    /* Schedule flash auto-off when stream transitions to false */
    static bool wasStreaming = false;
    if (wasStreaming && !isStreaming && !flashAutoOff) {
        flashAutoOff = true;
        flashOffMs   = millis();
    }
    wasStreaming = isStreaming;

    /* ----------------------------------------------------------
       DETECTION MODE  (default)
       Camera is GRAYSCALE, HTTP server is OFF.
       ---------------------------------------------------------- */
    if (!isStreaming) {

        // If transitioning BACK from streaming mode: stop server,
        // deinit JPEG camera, reinit grayscale camera.
        if (stream_httpd) {
            stop_stream_server();
            esp_camera_deinit();
            delay(CAMERA_REINIT_DELAY_MS);   // AI Thinker needs settle time
            prevFrameValid = false;           // Previous JPEG frame is stale
            if (!setupCamera(PIXFORMAT_GRAYSCALE, FRAMESIZE_QVGA)) {
                Serial.println("[ERROR] Camera reinit (detection mode) failed.");
                return;
            }
        }

        // Throttle motion detection — camera pipeline is heavy
        static unsigned long lastDetect = 0;
        if (millis() - lastDetect < MOTION_CHECK_INTERVAL_MS) return;
        lastDetect = millis();

        if (detectMotion()) {
            // Cooldown prevents packet flood on sustained motion
            static unsigned long lastMotionSend = 0;
            if (millis() - lastMotionSend > MOTION_SEND_COOLDOWN_MS) {
                myData.userID    = 0;    // Identity unknown until face verify
                myData.location  = 3;   // 3 = intruder/motion detected
                myData.command   = 0;
                myData.timestamp = millis();
                bool ok = masterNode->sendMessage((uint8_t *)&myData, sizeof(myData));
                if (ok) {
                    lastMotionSend = millis();
                    Serial.println("[ALERT] Motion alert sent to NODE_1.");
                } else {
                    Serial.println("[WARN] Motion alert send failed.");
                }
            }
        }

        // Heartbeat moved outside this block — see below.
    }

    /* ----------------------------------------------------------
       STREAMING MODE
       Camera is JPEG, HTTP server is ON.
       Entered when NODE_1 sends command=1.
       Exited when NODE_1 sends command=2.
       ---------------------------------------------------------- */
    else {

        // First time entering streaming mode: reinit camera for JPEG,
        // then start the HTTP server.
        if (!stream_httpd) {
            // Free motion detection buffer before switching pixel format
            if (prevFrame) {
                free(prevFrame);
                prevFrame      = nullptr;
                prevFrameValid = false;
            }
            esp_camera_deinit();
            delay(CAMERA_REINIT_DELAY_MS);  // AI Thinker PWDN settle

            if (setupCamera(PIXFORMAT_JPEG, FRAMESIZE_QVGA)) {
                delay(100);  // Extra settle before starting HTTP server
                start_stream_server();

                // Notify NODE_1 that stream is now live (with our IP)
                myData.command   = 1;
                myData.location  = 2;
                myData.timestamp = millis();
                masterNode->sendMessage((uint8_t *)&myData, sizeof(myData));
                myData.command = 0;  // Reset immediately — don't leave stale command

                Serial.printf("[INFO] Stream IP: http://%s/\n",
                              WiFi.localIP().toString().c_str());
                Serial.printf("[INFO] Capture URL: http://%s/capture\n",
                              WiFi.localIP().toString().c_str());
            } else {
                Serial.println("[ERROR] Camera reinit for streaming failed.");
                isStreaming = false;  // Fall back to detection mode
            }
        }
        // HTTP server task is autonomous after start — loop() has nothing to do here.
        // isStreaming will be cleared by the receive callback when NODE_1 sends command=2.
    }

    /* ── Heartbeat — runs in BOTH detection AND streaming mode ────────────
     * FIX: Previously this block lived inside if (!isStreaming) only.
     * Once NODE_4 entered streaming mode the heartbeat stopped, NODE_1's
     * 15-second timeout expired, and the dashboard showed NODE_4 as red.
     * Flash commands still worked because they are handled above the
     * if/else block. Moving the heartbeat here fixes the red indicator
     * without changing detection or streaming behaviour.                  */
    static unsigned long lastHB = 0;
    if (millis() - lastHB > HEARTBEAT_INTERVAL_MS) {
        myData.userID    = 0;
        myData.location  = 0;   // 0 = heartbeat (NOT 2 — that would confuse NODE_1)
        myData.command   = 0;
        myData.timestamp = millis();
        bool ok = masterNode->sendMessage((uint8_t *)&myData, sizeof(myData));
        // FIX: always update lastHB — same bug fixed in NODE_2.
        // A failed send previously left lastHB unchanged, hammering the
        // radio on every loop tick until the next successful send.
        lastHB = millis();
        if (!ok) Serial.println("[WARN] Heartbeat send failed — will retry in 10s.");
    }
}
