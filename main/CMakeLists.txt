idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp-sr esp_driver_i2s esp_timer esp_driver_gpio esp_driver_ledc freertos
)

# Optional: If you plan to play sounds (WAV files) later, 
# you might want to add 'esp_codec_dev' to PRIV_REQUIRES as well.
