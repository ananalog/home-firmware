#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "core_internal.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

// Point registry: current values, persisted settings (NVS "pts"), reports and the offline buffer.

static const char *TAG = "points";
static hp_value_t *s_values;
static bool *s_known;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_send_lock;  // guards the shared REPORT/STATE buffer

typedef struct {
    uint8_t point;
    float value;
    int64_t uptime_ms;
} offline_t;

#define OFFLINE_MAX 240
static offline_t s_offline[OFFLINE_MAX];
static size_t s_off_head, s_off_count;

size_t points_count(void) { return g_dev->n_points; }

static int index_of(uint8_t id)
{
    for (size_t i = 0; i < g_dev->n_points; i++)
        if (g_dev->points[i].id == id) return (int)i;
    return -1;
}

const home_point_t *points_find(uint8_t id)
{
    int i = index_of(id);
    return i < 0 ? NULL : &g_dev->points[i];
}

const hp_value_t *home_value(uint8_t id)
{
    int i = index_of(id);
    return i >= 0 && s_known[i] ? &s_values[i] : NULL;
}

static bool has_value(const hp_value_t *v) { return v->has_b || v->has_i || v->has_f || v->has_s; }

static void persist(uint8_t id, const hp_value_t *v)
{
    uint8_t buf[96];
    htlv_writer_t w;
    htlv_w_init(&w, buf, sizeof buf);
    hp_value_write(&w, v);
    if (w.overflow) return;
    nvs_handle_t h;
    if (nvs_open("pts", NVS_READWRITE, &h) != ESP_OK) return;
    char key[8];
    snprintf(key, sizeof key, "p%u", id);
    nvs_set_blob(h, key, buf, w.len);
    nvs_commit(h);
    nvs_close(h);
}

static bool load(uint8_t id, hp_value_t *out)
{
    nvs_handle_t h;
    if (nvs_open("pts", NVS_READONLY, &h) != ESP_OK) return false;
    char key[8];
    snprintf(key, sizeof key, "p%u", id);
    uint8_t buf[96];
    size_t n = sizeof buf;
    bool ok = nvs_get_blob(h, key, buf, &n) == ESP_OK && hp_value_read(out, buf, n) == HP_DECODE_OK && has_value(out);
    nvs_close(h);
    return ok;
}

void points_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_send_lock = xSemaphoreCreateMutex();
    s_values = calloc(g_dev->n_points, sizeof *s_values);
    s_known = calloc(g_dev->n_points, sizeof *s_known);
    for (size_t i = 0; i < g_dev->n_points; i++) {
        const home_point_t *p = &g_dev->points[i];
        hp_value_t v = p->def;
        if ((p->flags & HP_POINT_FLAGS_PERSIST) && load(p->id, &v)) ESP_LOGD(TAG, "restored %s", p->key);
        if (!has_value(&v)) continue;
        if (p->kind == HP_POINT_KIND_SETTING || p->kind == HP_POINT_KIND_ACTUATOR) {
            if (g_dev->on_set && g_dev->on_set(p->id, &v) != ESP_OK) v = p->def;
        }
        s_values[i] = v;
        s_known[i] = true;
    }
}

static void fill_def(hp_point_def_t *d, const home_point_t *p)
{
    memset(d, 0, sizeof *d);
    d->has_id = true;
    d->id = p->id;
    d->has_key = true;
    strlcpy(d->key, p->key, sizeof d->key);
    if (p->title) {
        d->has_title = true;
        strlcpy(d->title, p->title, sizeof d->title);
    }
    d->has_kind = d->has_type = d->has_ui = d->has_flags = true;
    d->kind = p->kind;
    d->type = p->type;
    d->ui = p->ui;
    d->flags = p->flags;
    if (p->unit) {
        d->has_unit = true;
        strlcpy(d->unit, p->unit, sizeof d->unit);
    }
    if (!isnan(p->min)) d->has_min = true, d->min = p->min;
    if (!isnan(p->max)) d->has_max = true, d->max = p->max;
    if (!isnan(p->step)) d->has_step = true, d->step = p->step;
    if (!isnan(p->thr_warn)) d->has_thr_warn = true, d->thr_warn = p->thr_warn;
    if (!isnan(p->thr_alarm)) d->has_thr_alarm = true, d->thr_alarm = p->thr_alarm;
    for (uint8_t i = 0; i < p->n_options && i < 8; i++) strlcpy(d->options[d->options_count++], p->options[i], sizeof d->options[0]);
    if (p->has_arg) {
        d->has_has_arg = d->has_arg_default = true;
        d->has_arg = true;
        d->arg_default = p->arg_default;
    }
}

void points_describe(htlv_writer_t *w)
{
    hp_point_def_t d;
    for (size_t i = 0; i < g_dev->n_points; i++) {
        fill_def(&d, &g_dev->points[i]);
        size_t mark = htlv_w_begin(w, 0x01);
        hp_point_def_write(w, &d);
        htlv_w_end(w, mark);
    }
}

static void write_sample(htlv_writer_t *w, uint8_t id, const hp_value_t *v, uint64_t ts)
{
    hp_sample_t s = {.has_point = true, .point = id, .has_value = true, .value = *v, .has_ts_ms = ts != 0, .ts_ms = ts};
    size_t mark = htlv_w_begin(w, 0x01);
    hp_sample_write(w, &s);
    htlv_w_end(w, mark);
}

void points_write_samples(htlv_writer_t *w, const uint8_t *ids, size_t n)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < g_dev->n_points; i++) {
        const home_point_t *p = &g_dev->points[i];
        if (!s_known[i] || p->kind == HP_POINT_KIND_ACTION) continue;
        bool want = n == 0;
        for (size_t j = 0; j < n && !want; j++) want = ids[j] == p->id;
        if (want) write_sample(w, p->id, &s_values[i], 0);
    }
    xSemaphoreGive(s_lock);
}

esp_err_t points_set(uint8_t id, const hp_value_t *in, hp_value_t *applied, uint8_t *err_code)
{
    int i = index_of(id);
    if (i < 0) {
        *err_code = HP_ERROR_CODE_NOT_FOUND;
        return ESP_ERR_NOT_FOUND;
    }
    const home_point_t *p = &g_dev->points[i];
    if (p->kind == HP_POINT_KIND_SENSOR || p->kind == HP_POINT_KIND_ACTION || (p->flags & HP_POINT_FLAGS_READONLY)) {
        *err_code = HP_ERROR_CODE_FORBIDDEN;
        return ESP_ERR_NOT_ALLOWED;
    }
    hp_value_t v = *in;
    float num = home_num(&v);
    bool type_ok = (p->type == HP_POINT_TYPE_BOOL && v.has_b) || (p->type == HP_POINT_TYPE_STR && v.has_s) ||
                   ((p->type == HP_POINT_TYPE_I32 || p->type == HP_POINT_TYPE_ENUM) && (v.has_i || v.has_f)) ||
                   (p->type == HP_POINT_TYPE_F32 && (v.has_f || v.has_i));
    if (!type_ok || (!isnan(num) && ((!isnan(p->min) && num < p->min) || (!isnan(p->max) && num > p->max))) ||
        (p->type == HP_POINT_TYPE_ENUM && (num < 0 || num >= p->n_options))) {
        *err_code = HP_ERROR_CODE_INVALID_VALUE;
        return ESP_ERR_INVALID_ARG;
    }
    if (p->type == HP_POINT_TYPE_I32 || p->type == HP_POINT_TYPE_ENUM) v = home_i32((int32_t)lroundf(num));
    if (p->type == HP_POINT_TYPE_F32) v = home_f32(num);
    if (g_dev->on_set) {
        esp_err_t err = g_dev->on_set(id, &v);
        if (err != ESP_OK) {
            *err_code = err == ESP_ERR_INVALID_ARG ? HP_ERROR_CODE_INVALID_VALUE : HP_ERROR_CODE_INTERNAL;
            return err;
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_values[i] = v;
    s_known[i] = true;
    xSemaphoreGive(s_lock);
    if (p->flags & HP_POINT_FLAGS_PERSIST) persist(id, &v);
    *applied = v;
    return ESP_OK;
}

static void send_samples(const uint8_t type, void (*fill)(htlv_writer_t *w, void *ctx), void *ctx)
{
    static uint8_t buf[MSG_MAX];
    xSemaphoreTake(s_send_lock, portMAX_DELAY);
    htlv_writer_t w;
    htlv_w_init(&w, buf, sizeof buf);
    hp_header_t h = {.ver = HP_HEADER_VERSION, .type = type, .flags = HP_FLAGS_NOACK, .req_id = 0};
    hp_header_write(&w, &h);
    fill(&w, ctx);
    if (!w.overflow) link_send(buf, w.len);
    xSemaphoreGive(s_send_lock);
}

static void fill_all(htlv_writer_t *w, void *ctx) { points_write_samples(w, NULL, 0); }

void points_send_state(void) { send_samples(HP_MSG_STATE, fill_all, NULL); }

typedef struct {
    uint8_t id;
    const hp_value_t *v;
} one_t;

static void fill_one(htlv_writer_t *w, void *ctx)
{
    one_t *o = ctx;
    write_sample(w, o->id, o->v, home_time_ms());
}

void home_report(uint8_t id, hp_value_t v)
{
    int i = index_of(id);
    if (i < 0) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_values[i] = v;
    s_known[i] = true;
    xSemaphoreGive(s_lock);
    const home_point_t *p = &g_dev->points[i];
    if (link_online()) {
        one_t o = {id, &v};
        send_samples(HP_MSG_REPORT, fill_one, &o);
    } else if ((p->flags & HP_POINT_FLAGS_HISTORY) && !isnan(home_num(&v))) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        size_t pos = (s_off_head + s_off_count) % OFFLINE_MAX;
        s_offline[pos] = (offline_t){id, home_num(&v), esp_timer_get_time() / 1000};
        if (s_off_count < OFFLINE_MAX) s_off_count++;
        else s_off_head = (s_off_head + 1) % OFFLINE_MAX;
        xSemaphoreGive(s_lock);
    }
}

void home_flush_reports(void) { points_send_state(); }

static void fill_offline(htlv_writer_t *w, void *ctx)
{
    size_t *sent = ctx;
    int64_t now_up = esp_timer_get_time() / 1000;
    uint64_t now = home_time_ms();
    for (size_t k = 0; k < 20 && s_off_count > 0; k++) {
        offline_t *o = &s_offline[s_off_head];
        hp_value_t v = home_f32(o->value);
        uint64_t ts = now ? now - (uint64_t)(now_up - o->uptime_ms) : 0;
        write_sample(w, o->point, &v, ts);
        s_off_head = (s_off_head + 1) % OFFLINE_MAX;
        s_off_count--;
        (*sent)++;
    }
}

void points_flush_offline(void)
{
    size_t total = 0;
    while (s_off_count > 0 && link_online()) {
        size_t sent = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        send_samples(HP_MSG_REPORT, fill_offline, &sent);
        xSemaphoreGive(s_lock);
        total += sent;
        if (!sent) break;
    }
    if (total) ESP_LOGI(TAG, "sent %u buffered samples", (unsigned)total);
}

void home_event(uint8_t kind, uint8_t point, const hp_value_t *value, const char *text)
{
    if (!link_online()) return;
    hp_event_req_t e = {.has_kind = true, .kind = kind, .has_point = point != 0, .point = point};
    if (value) e.has_value = true, e.value = *value;
    if (text) e.has_text = true, strlcpy(e.text, text, sizeof e.text);
    uint64_t t = home_time_ms();
    e.has_ts_ms = t != 0;
    e.ts_ms = t;
    uint8_t buf[192];
    size_t n;
    MSG_BUILD(buf, sizeof buf, &n, HP_MSG_EVENT, HP_FLAGS_NOACK, 0, hp_event_req_write, &e);
    link_send(buf, n);
}
