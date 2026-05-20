idf_component_register(
    SRCS
        "main.c"
        "wifi.c"
        "espnow.c"
        "http_client.c"
        "face_verify.c"
        "blynk.c"

    INCLUDE_DIRS
        "."
    EMBED_FILES "dashboard.html"
    REQUIRES
    freertos
    nvs_flash
    heap
    log
    esp_timer
    esp_wifi
    esp_netif
    esp_event
    esp_http_client
    esp_http_server
    mbedtls
    espressif__cjson
    esp_driver_gpio      # driver/gpio.h
    esp_driver_i2s       # driver/i2s_std.h
    esp_driver_ledc      # driver/ledc.h  <-- add this now
    esp-sr               # esp_afe_sr_iface.h, esp_mn_models.h, etc.   # face_verify.c likely needs this
    espressif__esp-dsp    # if using DSP in face_verify
)
