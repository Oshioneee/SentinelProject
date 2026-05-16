/* face_verify.h  -  Face++ cloud verification (ESP-IDF 5.x) */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

/*
 * face_verify()
 * POSTs a JPEG to the Face++ /v3/search API.
 * On success: out_name = "Caleb"/"Emmanuel"/etc. or "Unknown"
 * Returns ESP_OK even when no match; ESP_FAIL only on network error.
 *
 * Face++ enrollment (do once per person):
 *   Console -> FaceSet "HomeResidents" -> Add Face
 *   Set user_id = "Caleb" / "Emmanuel" / "Mary" / "Princess"
 */
esp_err_t face_verify(const uint8_t *jpeg_buf, size_t jpeg_len,
                      char *out_name, size_t name_buf_size,
                      float *out_confidence);
