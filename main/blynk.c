/*
 * blynk.c  -  Blynk IoT HTTPS REST API (ESP-IDF 5.x)
 *
 * BUG FIX applied here:
 *   All string values written to Blynk are now URL-encoded before
 *   being embedded in the query string.  Without encoding, any event
 *   label containing a space — e.g. "Caleb verified at Main Door" —
 *   produced an invalid URL that Blynk silently rejected, so the
 *   widget never updated.
 *
 *   Affected functions (both fixed):
 *     blynk_write_str()    event labels, face results, node names
 *     blynk_set_property() Imgbb image URL pushed to V7 Image Widget
 *
 *   Characters that MUST be encoded in query-string values:
 *     space -> %20,  & -> %26,  = -> %3D,  + -> %2B,
 *     # -> %23,  % -> %25,  : -> %3A,  / -> %2F
 */
#include "blynk.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
#include "esp_crt_bundle.h"
static const char *TAG = "BLYNK";
#define API "https://" BLYNK_HOST "/external/api"

/* ----------------------------------------------------------------
   url_encode()
   Percent-encodes src into dst (max dst_size bytes incl. NUL).
   RFC 3986 §2.3 unreserved chars (A-Za-z0-9 - _ . ~) pass through.
   Everything else becomes %XX.
   Returns bytes written (excl. NUL), or -1 if dst would overflow.
   ---------------------------------------------------------------- */
static int url_encode(const char *src, char *dst, size_t dst_size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            if (out + 1 >= dst_size) return -1;
            dst[out++] = (char)c;
        } else {
            if (out + 3 >= dst_size) return -1;
            dst[out++] = '%';
            dst[out++] = hex[(c >> 4) & 0x0F];
            dst[out++] = hex[ c       & 0x0F];
        }
    }
    dst[out] = '\0';
    return (int)out;
}

/* ----------------------------------------------------------------
   Internal: one HTTPS GET, returns HTTP status code or -1.
   ---------------------------------------------------------------- */
static int blynk_get(const char *url) {
    esp_http_client_config_t cfg = {
        .url               = url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = 8000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_err_t err  = esp_http_client_perform(c);
    int       code = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    return (err == ESP_OK) ? code : -1;
}

/* ----------------------------------------------------------------
   Public API
   ---------------------------------------------------------------- */

esp_err_t blynk_init(void) {
    char url[256];
    snprintf(url, sizeof(url),
             API "/isHardwareConnected?token=%s", BLYNK_AUTH_TOKEN);
    int code = blynk_get(url);
    if (code == 200) { ESP_LOGI(TAG, "Blynk OK."); }
    else             { ESP_LOGW(TAG, "Blynk ping HTTP %d (non-fatal).", code); }
    return ESP_OK;
}

esp_err_t blynk_write_int(int vpin, int value) {
    /* Integers are digits+minus — no encoding needed */
    char url[256];
    snprintf(url, sizeof(url),
             API "/update?token=%s&V%d=%d", BLYNK_AUTH_TOKEN, vpin, value);
    int code = blynk_get(url);
    if (code != 200) {
        ESP_LOGE(TAG, "write_int V%d=%d -> HTTP %d", vpin, value, code);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t blynk_write_float(int vpin, float value) {
    /* Floats are digits+dot+minus — no encoding needed */
    char url[256];
    snprintf(url, sizeof(url),
             API "/update?token=%s&V%d=%.2f", BLYNK_AUTH_TOKEN, vpin, (double)value);
    int code = blynk_get(url);
    if (code != 200) {
        ESP_LOGE(TAG, "write_float V%d=%.2f -> HTTP %d", vpin, (double)value, code);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t blynk_write_str(int vpin, const char *value) {
    if (!value) return ESP_ERR_INVALID_ARG;

    /*
     * FIX: encode before embedding.
     * Worst case: 3 bytes out per 1 byte in (all special chars).
     * Typical event labels <= 96 chars → 288 encoded max.
     * 512 is sufficient.
     */
    char encoded[512];
    if (url_encode(value, encoded, sizeof(encoded)) < 0) {
        ESP_LOGE(TAG, "write_str V%d: value too long to encode.", vpin);
        return ESP_ERR_INVALID_SIZE;
    }

    /* URL buffer: API_base(55) + token(32) + "&V__="(6) + encoded(512) + NUL */
    char url[700];
    snprintf(url, sizeof(url),
             API "/update?token=%s&V%d=%s", BLYNK_AUTH_TOKEN, vpin, encoded);
    int code = blynk_get(url);
    if (code != 200) {
        ESP_LOGE(TAG, "write_str V%d='%s' -> HTTP %d", vpin, value, code);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t blynk_set_property(int vpin, const char *property, const char *value) {
    if (!property || !value) return ESP_ERR_INVALID_ARG;

    /*
     * FIX: encode the value (typically an Imgbb HTTPS URL).
     * "https://i.ibb.co/abc/img.jpg" -> "https%3A%2F%2Fi.ibb.co%2Fabc%2Fimg.jpg"
     * This makes the URL unambiguous and RFC-compliant.
     * Imgbb URLs are ~80 chars → encoded max ~240.  768 is ample.
     */
    char enc_value[768];
    if (url_encode(value, enc_value, sizeof(enc_value)) < 0) {
        ESP_LOGE(TAG, "set_property V%d: value too long to encode.", vpin);
        return ESP_ERR_INVALID_SIZE;
    }

    /* URL buffer: base + token + pin + property + encoded_value */
    char url[1100];
    snprintf(url, sizeof(url),
             API "/update/property?token=%s&pin=V%d&property=%s&value=%s",
             BLYNK_AUTH_TOKEN, vpin, property, enc_value);
    int code = blynk_get(url);
    if (code != 200) {
        ESP_LOGE(TAG, "set_property V%d %s -> HTTP %d", vpin, property, code);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "V%d property '%s' updated.", vpin, property);
    return ESP_OK;
}
