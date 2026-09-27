#include <string.h>

#include "core_internal.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

// OTA over the protocol (TCP from the server or BLE from the phone): OTA_BEGIN erases the next slot,
// OTA_DATA writes chunks in order, OTA_END checks SHA-256 and switches the boot partition.
// The new firmware boots in PENDING_VERIFY state and rolls back unless confirmed (see core.c).

static const char *TAG = "ota";
static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static mbedtls_sha256_context s_sha;
static uint8_t s_expected[32];
static uint32_t s_size, s_received;
static uint8_t s_state = HP_OTA_STATE_IDLE;
static uint8_t s_error;
static int64_t s_last_ms;
static int s_last_pct = -1;
static uint8_t s_out[64];

bool ota_busy(void) { return s_state == HP_OTA_STATE_RECEIVING || s_state == HP_OTA_STATE_VERIFYING; }

void ota_init(void) {}

static void finish(uint8_t state, uint8_t err)
{
    if (s_handle) {
        esp_ota_abort(s_handle);
        s_handle = 0;
    }
    mbedtls_sha256_free(&s_sha);
    s_state = state;
    s_error = err;
    core_ui(HOME_UI_OTA, -1);
}

static void reply_error(home_transport_t tr, const hp_header_t *h, uint8_t code, const char *text)
{
    hp_error_body_t e = {.has_code = true, .code = code, .has_text = text != NULL};
    if (text) strlcpy(e.text, text, sizeof e.text);
    uint8_t buf[128];
    size_t n;
    MSG_BUILD(buf, sizeof buf, &n, h->type, HP_FLAGS_RESP | HP_FLAGS_ERR, h->req_id, hp_error_body_write, &e);
    transport_send(tr, buf, n);
}

static void begin(home_transport_t tr, const hp_header_t *h, const uint8_t *body, size_t bn)
{
    hp_ota_begin_req_t r;
    if (hp_ota_begin_req_read(&r, body, bn) != HP_DECODE_OK || !r.has_size || !r.has_sha256 || r.sha256_len != 32) {
        reply_error(tr, h, HP_ERROR_CODE_BAD_REQUEST, "size and sha256 required");
        return;
    }
    if (r.has_model && r.model != g_dev->model) {
        reply_error(tr, h, HP_ERROR_CODE_OTA_WRONG_MODEL, "image is for another model");
        return;
    }
    // Resume an interrupted transfer of the same image.
    if (s_state == HP_OTA_STATE_RECEIVING && memcmp(s_expected, r.sha256, 32) == 0 && s_size == r.size) {
        hp_ota_begin_resp_t resp = {.has_chunk = true, .chunk = HP_OTA_CHUNK, .has_resume_from = true, .resume_from = s_received};
        size_t n;
        MSG_BUILD(s_out, sizeof s_out, &n, h->type, HP_FLAGS_RESP, h->req_id, hp_ota_begin_resp_write, &resp);
        transport_send(tr, s_out, n);
        ESP_LOGI(TAG, "resume at %lu", (unsigned long)s_received);
        return;
    }
    if (ota_busy()) finish(HP_OTA_STATE_IDLE, 0);
    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part || r.size > s_part->size) {
        reply_error(tr, h, HP_ERROR_CODE_OTA_BAD_IMAGE, "image does not fit the OTA slot");
        return;
    }
    ESP_LOGI(TAG, "begin %s: %lu bytes into %s", r.has_version ? r.version : "?", (unsigned long)r.size, s_part->label);
    esp_err_t err = esp_ota_begin(s_part, r.size, &s_handle);  // erases the needed part of the slot
    if (err != ESP_OK) {
        reply_error(tr, h, HP_ERROR_CODE_STORAGE, esp_err_to_name(err));
        return;
    }
    mbedtls_sha256_init(&s_sha);
    mbedtls_sha256_starts(&s_sha, 0);
    memcpy(s_expected, r.sha256, 32);
    s_size = r.size;
    s_received = 0;
    s_state = HP_OTA_STATE_RECEIVING;
    s_error = 0;
    s_last_pct = -1;
    s_last_ms = esp_timer_get_time() / 1000;
    core_ui(HOME_UI_OTA, 0);
    hp_ota_begin_resp_t resp = {.has_chunk = true, .chunk = HP_OTA_CHUNK, .has_resume_from = true, .resume_from = 0};
    size_t n;
    MSG_BUILD(s_out, sizeof s_out, &n, h->type, HP_FLAGS_RESP, h->req_id, hp_ota_begin_resp_write, &resp);
    transport_send(tr, s_out, n);
}

static void data(home_transport_t tr, const hp_header_t *h, const uint8_t *body, size_t bn)
{
    hp_ota_data_req_t r;
    if (s_state != HP_OTA_STATE_RECEIVING) {
        reply_error(tr, h, HP_ERROR_CODE_BAD_REQUEST, "no OTA in progress");
        return;
    }
    if (hp_ota_data_req_read(&r, body, bn) != HP_DECODE_OK || !r.has_offset || !r.has_data) {
        reply_error(tr, h, HP_ERROR_CODE_BAD_REQUEST, NULL);
        return;
    }
    if (r.offset != s_received || s_received + r.data_len > s_size) {
        char t[48];
        snprintf(t, sizeof t, "expected offset %lu", (unsigned long)s_received);
        reply_error(tr, h, HP_ERROR_CODE_BAD_REQUEST, t);
        return;
    }
    esp_err_t err = esp_ota_write(s_handle, r.data, r.data_len);
    if (err != ESP_OK) {
        finish(HP_OTA_STATE_FAILED, HP_ERROR_CODE_STORAGE);
        reply_error(tr, h, HP_ERROR_CODE_STORAGE, esp_err_to_name(err));
        return;
    }
    mbedtls_sha256_update(&s_sha, r.data, r.data_len);
    s_received += r.data_len;
    s_last_ms = esp_timer_get_time() / 1000;
    int pct = (int)((uint64_t)s_received * 100 / s_size);
    if (pct / 5 != s_last_pct / 5) {
        s_last_pct = pct;
        core_ui(HOME_UI_OTA, pct);
    }
    hp_ota_data_resp_t resp = {.has_next_offset = true, .next_offset = s_received};
    size_t n;
    MSG_BUILD(s_out, sizeof s_out, &n, h->type, HP_FLAGS_RESP, h->req_id, hp_ota_data_resp_write, &resp);
    transport_send(tr, s_out, n);
}

static void end(home_transport_t tr, const hp_header_t *h)
{
    if (s_state != HP_OTA_STATE_RECEIVING || s_received != s_size) {
        reply_error(tr, h, HP_ERROR_CODE_OTA_BAD_IMAGE, "incomplete image");
        return;
    }
    s_state = HP_OTA_STATE_VERIFYING;
    uint8_t sha[32];
    mbedtls_sha256_finish(&s_sha, sha);
    if (memcmp(sha, s_expected, 32) != 0) {
        finish(HP_OTA_STATE_FAILED, HP_ERROR_CODE_OTA_HASH_MISMATCH);
        reply_error(tr, h, HP_ERROR_CODE_OTA_HASH_MISMATCH, "sha256 mismatch");
        return;
    }
    esp_err_t err = esp_ota_end(s_handle);  // validates the image structure
    s_handle = 0;
    if (err == ESP_OK) err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        finish(HP_OTA_STATE_FAILED, HP_ERROR_CODE_OTA_BAD_IMAGE);
        reply_error(tr, h, HP_ERROR_CODE_OTA_BAD_IMAGE, esp_err_to_name(err));
        return;
    }
    mbedtls_sha256_free(&s_sha);
    s_state = HP_OTA_STATE_DONE;
    uint8_t b[HP_HEADER_SIZE];
    htlv_writer_t w;
    htlv_w_init(&w, b, sizeof b);
    hp_header_t rh = {.ver = HP_HEADER_VERSION, .type = h->type, .flags = HP_FLAGS_RESP, .req_id = h->req_id};
    hp_header_write(&w, &rh);
    transport_send(tr, b, w.len);
    ESP_LOGI(TAG, "image ok, rebooting into %s", s_part->label);
    core_ui(HOME_UI_OTA, 100);
    vTaskDelay(pdMS_TO_TICKS(500));
    home_restart("OTA finished");
}

void ota_handle(home_transport_t tr, const hp_header_t *h, const uint8_t *body, size_t bn)
{
    // Abandon a transfer that stalled for 2 minutes.
    if (s_state == HP_OTA_STATE_RECEIVING && esp_timer_get_time() / 1000 - s_last_ms > 120000) finish(HP_OTA_STATE_FAILED, HP_ERROR_CODE_TIMEOUT);
    switch (h->type) {
    case HP_MSG_OTA_BEGIN: begin(tr, h, body, bn); break;
    case HP_MSG_OTA_DATA: data(tr, h, body, bn); break;
    case HP_MSG_OTA_END: end(tr, h); break;
    case HP_MSG_OTA_ABORT: {
        if (ota_busy()) finish(HP_OTA_STATE_IDLE, 0);
        uint8_t b[HP_HEADER_SIZE];
        htlv_writer_t w;
        htlv_w_init(&w, b, sizeof b);
        hp_header_t rh = {.ver = HP_HEADER_VERSION, .type = h->type, .flags = HP_FLAGS_RESP, .req_id = h->req_id};
        hp_header_write(&w, &rh);
        transport_send(tr, b, w.len);
        break;
    }
    case HP_MSG_OTA_STATUS: {
        hp_ota_status_resp_t r = {.has_state = true, .state = s_state, .has_received = true, .received = s_received, .has_error = true, .error = s_error};
        size_t n;
        MSG_BUILD(s_out, sizeof s_out, &n, h->type, HP_FLAGS_RESP, h->req_id, hp_ota_status_resp_write, &r);
        transport_send(tr, s_out, n);
        break;
    }
    }
}
