#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core_internal.h"
#include "esp_log.h"
#include "freertos/queue.h"

// Forwards ESP-IDF log lines ("I (123) tag: text") to the server as LOG messages.
// The level is set by the server (HELLO answer) or CFG_SET; lines are queued and sent by the link task.

typedef struct {
    uint8_t level;
    char tag[17];
    char text[160];
    uint64_t ts;
} line_t;

static QueueHandle_t s_q;
static vprintf_like_t s_orig;

static uint8_t level_of(char c)
{
    switch (c) {
    case 'E': return HP_LOG_LEVEL_ERROR;
    case 'W': return HP_LOG_LEVEL_WARN;
    case 'I': return HP_LOG_LEVEL_INFO;
    case 'D': return HP_LOG_LEVEL_DEBUG;
    case 'V': return HP_LOG_LEVEL_VERBOSE;
    default: return HP_LOG_LEVEL_NONE;
    }
}

static int hook(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int r = s_orig ? s_orig(fmt, ap) : vprintf(fmt, ap);
    if (s_q && link_online() && g_adopted) {
        char buf[200];
        vsnprintf(buf, sizeof buf, fmt, copy);
        // Skip colour codes.
        const char *p = buf;
        if (*p == '\033') {
            p = strchr(p, 'm');
            p = p ? p + 1 : buf;
        }
        uint8_t lvl = level_of(*p);
        if (lvl != HP_LOG_LEVEL_NONE && lvl <= g_cfg.log_level) {
            line_t l = {.level = lvl, .ts = home_time_ms()};
            const char *close = strchr(p, ')');
            const char *colon = close ? strstr(close, ": ") : NULL;
            if (close && colon) {
                size_t tl = (size_t)(colon - close - 2);
                if (tl >= sizeof l.tag) tl = sizeof l.tag - 1;
                memcpy(l.tag, close + 2, tl);
                strlcpy(l.text, colon + 2, sizeof l.text);
            } else strlcpy(l.text, p, sizeof l.text);
            size_t n = strlen(l.text);
            while (n && (l.text[n - 1] == '\n' || l.text[n - 1] == '\r' || l.text[n - 1] == 'm' || l.text[n - 1] == '\033')) {
                if (l.text[n - 1] == 'm') {  // trailing "\033[0m"
                    char *esc = strrchr(l.text, '\033');
                    if (!esc) break;
                    *esc = 0;
                    n = strlen(l.text);
                    continue;
                }
                l.text[--n] = 0;
            }
            xQueueSend(s_q, &l, 0);
        }
    }
    va_end(copy);
    return r;
}

void logfwd_start(void)
{
    s_q = xQueueCreate(12, sizeof(line_t));
    s_orig = esp_log_set_vprintf(hook);
}

void logfwd_drain(void)
{
    line_t l;
    static uint8_t buf[256];
    while (s_q && xQueueReceive(s_q, &l, 0) == pdTRUE) {
        hp_log_req_t m = {.has_level = true, .level = l.level, .has_tag = true, .has_text = true, .has_ts_ms = l.ts != 0, .ts_ms = l.ts};
        strlcpy(m.tag, l.tag, sizeof m.tag);
        strlcpy(m.text, l.text, sizeof m.text);
        size_t n;
        MSG_BUILD(buf, sizeof buf, &n, HP_MSG_LOG, HP_FLAGS_NOACK, 0, hp_log_req_write, &m);
        if (n) link_send(buf, n);
    }
}
