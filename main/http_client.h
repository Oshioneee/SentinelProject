/* http_client.h  -  JPEG fetch + Imgbb upload (ESP-IDF 5.x) */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/* GET NODE4_CAPTURE_URL -> JPEG in PSRAM. Caller must free(*out_buf). */
esp_err_t http_fetch_jpeg(uint8_t **out_buf, size_t *out_len);

/* Base64-encode JPEG and POST to Imgbb. out_url = public HTTPS URL. */
esp_err_t http_upload_imgbb(const uint8_t *jpeg_buf, size_t jpeg_len,
                             char *out_url, size_t url_buf_size);
