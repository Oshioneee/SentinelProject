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

    REQUIRES
        freertos
        nvs_flash
        heap
        log
        esp_timer        # <-- added: provides esp_timer.h
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp_http_server
        mbedtls
        espressif__cjson
        driver
        esp_driver_gpio
        esp_driver_i2s
        esp_driver_ledc
        esp-sr
    
    EMBED_FILES
        "dashboard.html"
)