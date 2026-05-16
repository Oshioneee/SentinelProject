/*
 * http_client.c  -  JPEG fetch + Imgbb upload (ESP-IDF 5.x)
 *
 * Both functions use a growing PSRAM response buffer via the
 * esp_http_client event callback pattern.
 */
#include "http_client.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include "esp_crt_bundle.h"
static const char *TAG = "HTTP";

/* Response accumulator - grows dynamically in PSRAM */
typedef struct { uint8_t *buf; size_t len; size_t cap; } resp_t;

static esp_err_t on_data(esp_http_client_event_t *evt) {
    resp_t *r = (resp_t *)evt->user_data;
    if (!r) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        if (r->len + evt->data_len >= r->cap) {
            size_t nc = r->cap + evt->data_len + 4096;
            uint8_t *nb = heap_caps_realloc(r->buf, nc, MALLOC_CAP_SPIRAM);
            if (!nb) return ESP_ERR_NO_MEM;
            r->buf = nb; r->cap = nc;
        }
        memcpy(r->buf + r->len, evt->data, evt->data_len);
        r->len += evt->data_len;
    }
    return ESP_OK;
}

/* ---- Fetch JPEG from NODE_4 /capture ------------------------- */
esp_err_t http_fetch_jpeg(uint8_t **out_buf, size_t *out_len) {
    *out_buf = NULL; *out_len = 0;

    resp_t r = { heap_caps_malloc(65536, MALLOC_CAP_SPIRAM), 0, 65536 };
    if (!r.buf) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url = NODE4_CAPTURE_URL, .timeout_ms = 8000,
        .event_handler = on_data, .user_data = &r, .buffer_size = 4096
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { heap_caps_free(r.buf); return ESP_FAIL; }

    esp_err_t err = esp_http_client_perform(c);
    int code = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);

    if (err != ESP_OK || code != 200 || r.len < 50) {
        ESP_LOGE(TAG, "JPEG fetch: err=%s HTTP=%d bytes=%u",
                 esp_err_to_name(err), code, (unsigned)r.len);
        heap_caps_free(r.buf); return ESP_FAIL;
    }
    ESP_LOGI(TAG, "JPEG: %u bytes.", (unsigned)r.len);
    *out_buf = r.buf; *out_len = r.len;
    return ESP_OK;
}

/* ---- Upload JPEG to Imgbb ------------------------------------ */
esp_err_t http_upload_imgbb(const uint8_t *jpeg, size_t jlen,
                             char *out_url, size_t url_sz) {
    out_url[0] = '\0';

    /* 1. Base64 encode */
    size_t b64c = ((jlen + 2) / 3) * 4 + 1;
    char *b64 = heap_caps_malloc(b64c, MALLOC_CAP_SPIRAM);
    if (!b64) return ESP_ERR_NO_MEM;
    size_t b64l = 0;
    if (mbedtls_base64_encode((unsigned char*)b64, b64c, &b64l, jpeg, jlen) != 0) {
        heap_caps_free(b64); return ESP_FAIL;
    }
    b64[b64l] = '\0';

    /* 2. Build POST body */
    size_t bodyc = b64l + strlen(IMGBB_API_KEY) + 32;
    char *body = heap_caps_malloc(bodyc, MALLOC_CAP_SPIRAM);
    if (!body) { heap_caps_free(b64); return ESP_ERR_NO_MEM; }
    snprintf(body, bodyc, "key=%s&image=%s", IMGBB_API_KEY, b64);
    heap_caps_free(b64);

    /* 3. POST */
    resp_t r = { heap_caps_malloc(2048, MALLOC_CAP_SPIRAM), 0, 2048 };
    if (!r.buf) { heap_caps_free(body); return ESP_ERR_NO_MEM; }

    esp_http_client_config_t cfg = {
        .url = IMGBB_UPLOAD_URL, .method = HTTP_METHOD_POST,
        .timeout_ms = 20000, .event_handler = on_data, .user_data = &r,
        .crt_bundle_attach = esp_crt_bundle_attach
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(c, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(c);
    int code = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    heap_caps_free(body);

    if (err != ESP_OK || code != 200) {
        ESP_LOGE(TAG, "Imgbb: err=%s HTTP=%d", esp_err_to_name(err), code);
        heap_caps_free(r.buf); return ESP_FAIL;
    }

    /* 4. Parse JSON: {"data":{"url":"..."},"success":true} */
    if (r.len >= r.cap) r.len = r.cap - 1;
    r.buf[r.len] = '\0';

    cJSON *root = cJSON_Parse((char*)r.buf);
    heap_caps_free(r.buf);
    if (!root) { ESP_LOGE(TAG, "Imgbb JSON parse error."); return ESP_FAIL; }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    cJSON *url  = data ? cJSON_GetObjectItem(data, "url") : NULL;
    if (!cJSON_IsString(url)) { cJSON_Delete(root); return ESP_FAIL; }

    strncpy(out_url, url->valuestring, url_sz - 1);
    out_url[url_sz - 1] = '\0';
    cJSON_Delete(root);
    ESP_LOGI(TAG, "Imgbb URL: %s", out_url);
    return ESP_OK;
}
