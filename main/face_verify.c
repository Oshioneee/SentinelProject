/*
 * face_verify.c  -  Face++ Search API (ESP-IDF 5.x)
 *
 * POSTs a base64-encoded JPEG to Face++ /v3/search.
 * Parses JSON to extract: user_id label + confidence score.
 * Returns "Unknown" if no face detected, no match, or low confidence.
 *
 * Face++ response structure (success):
 * {
 *   "faces": [{"face_token": "abc..."}],      <- faces found in image
 *   "results": [{
 *     "confidence": 87.4,
 *     "user_id": "Caleb",                     <- enrolled label
 *     "face_token": "def..."
 *   }]
 * }
 */
#include "face_verify.h"
#include "config.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "cJSON.h"
#include <string.h>
#include "esp_crt_bundle.h"
static const char *TAG = "FACE++";

typedef struct { uint8_t *buf; size_t len; size_t cap; } fpp_resp_t;

static esp_err_t fpp_on_data(esp_http_client_event_t *evt) {
    fpp_resp_t *r = (fpp_resp_t *)evt->user_data;
    if (!r) return ESP_OK;
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        if (r->len + evt->data_len >= r->cap) {
            size_t nc = r->cap + evt->data_len + 2048;
            uint8_t *nb = heap_caps_realloc(r->buf, nc, MALLOC_CAP_SPIRAM);
            if (!nb) return ESP_ERR_NO_MEM;
            r->buf = nb; r->cap = nc;
        }
        memcpy(r->buf + r->len, evt->data, evt->data_len);
        r->len += evt->data_len;
    }
    return ESP_OK;
}

esp_err_t face_verify(const uint8_t *jpeg, size_t jlen,
                      char *out_name, size_t name_sz, float *out_conf) {
    /* Safe defaults */
    strncpy(out_name, "Unknown", name_sz - 1);
    out_name[name_sz - 1] = '\0';
    *out_conf = 0.0f;

    /* 1. Base64 encode JPEG */
    size_t b64c = ((jlen + 2) / 3) * 4 + 1;
    char *b64 = heap_caps_malloc(b64c, MALLOC_CAP_SPIRAM);
    if (!b64) return ESP_ERR_NO_MEM;
    size_t b64l = 0;
    if (mbedtls_base64_encode((unsigned char*)b64, b64c, &b64l, jpeg, jlen) != 0) {
        heap_caps_free(b64); return ESP_FAIL;
    }
    b64[b64l] = '\0';

    /* 2. Build POST body */
    size_t bodyc = b64l + strlen(FACEPP_API_KEY)
                        + strlen(FACEPP_API_SECRET)
                        + strlen(FACEPP_FACESET_TOKEN) + 128;
    char *body = heap_caps_malloc(bodyc, MALLOC_CAP_SPIRAM);
    if (!body) { heap_caps_free(b64); return ESP_ERR_NO_MEM; }
    snprintf(body, bodyc,
             "api_key=%s&api_secret=%s&faceset_token=%s&image_base64=%s",
             FACEPP_API_KEY, FACEPP_API_SECRET, FACEPP_FACESET_TOKEN, b64);
    heap_caps_free(b64);

    /* 3. HTTP POST */
    fpp_resp_t r = { heap_caps_malloc(4096, MALLOC_CAP_SPIRAM), 0, 4096 };
    if (!r.buf) { heap_caps_free(body); return ESP_ERR_NO_MEM; }

    esp_http_client_config_t cfg = {
        .url = FACEPP_SEARCH_URL, .method = HTTP_METHOD_POST,
        .timeout_ms = 20000, .event_handler = fpp_on_data, .user_data = &r,
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
        ESP_LOGE(TAG, "HTTP: err=%s code=%d", esp_err_to_name(err), code);
        heap_caps_free(r.buf); return ESP_FAIL;
    }

    /* 4. Parse JSON */
    if (r.len >= r.cap) r.len = r.cap - 1;
    r.buf[r.len] = '\0';

    cJSON *root = cJSON_Parse((char*)r.buf);
    heap_caps_free(r.buf);
    if (!root) { ESP_LOGE(TAG, "JSON parse error."); return ESP_FAIL; }

    /* Check a face was actually detected */
    cJSON *faces = cJSON_GetObjectItem(root, "faces");
    if (!faces || cJSON_GetArraySize(faces) == 0) {
        ESP_LOGW(TAG, "No face detected in image.");
        cJSON_Delete(root); return ESP_OK;
    }

    /* Check for a match in the FaceSet */
    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!results || cJSON_GetArraySize(results) == 0) {
        ESP_LOGW(TAG, "Face detected but no FaceSet match.");
        cJSON_Delete(root); return ESP_OK;
    }

    /* Extract top match */
    cJSON *match = cJSON_GetArrayItem(results, 0);
    cJSON *conf  = cJSON_GetObjectItem(match, "confidence");
    cJSON *uid   = cJSON_GetObjectItem(match, "user_id");

    float c_val = conf ? (float)conf->valuedouble : 0.0f;
    *out_conf = c_val;
    ESP_LOGI(TAG, "Match: %s  conf=%.1f%%",
             cJSON_IsString(uid) ? uid->valuestring : "none", (double)c_val);

    /* Only accept if above minimum confidence threshold */
    if (c_val >= FACEPP_MIN_CONFIDENCE && cJSON_IsString(uid)) {
        strncpy(out_name, uid->valuestring, name_sz - 1);
        out_name[name_sz - 1] = '\0';
    }

    cJSON_Delete(root);
    return ESP_OK;
}
