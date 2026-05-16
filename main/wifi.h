/* wifi.h  -  WiFi STA (ESP-IDF 5.x) */
#pragma once
#include "esp_err.h"
/* Connect to AP in config.h, block until IP assigned (max 30s).
   Must be called BEFORE espnow_init(). */
esp_err_t wifi_init_sta(void);
