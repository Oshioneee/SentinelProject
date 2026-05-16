/* blynk.h  -  Blynk IoT HTTPS REST API (ESP-IDF 5.x) */
#pragma once
#include "esp_err.h"

/*
 * All string values passed to blynk_write_str() and blynk_set_property()
 * are automatically URL percent-encoded before being sent.
 * You do NOT need to pre-encode values — pass them as plain strings.
 */

/* Ping Blynk on startup; non-fatal if offline. */
esp_err_t blynk_init(void);

/* Write integer to virtual pin (LED widget, value display). */
esp_err_t blynk_write_int(int vpin, int value);

/* Write float to virtual pin (gauge widget). */
esp_err_t blynk_write_float(int vpin, float value);

/*
 * Write string to virtual pin (label, terminal).
 * Value is URL-encoded automatically — pass plain text with spaces.
 */
esp_err_t blynk_write_str(int vpin, const char *value);

/*
 * Set a widget property (e.g. property="url" for Image Widget).
 * Value is URL-encoded automatically — pass the raw Imgbb HTTPS URL.
 */
esp_err_t blynk_set_property(int vpin, const char *property, const char *value);
