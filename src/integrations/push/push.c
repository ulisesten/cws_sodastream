#define _POSIX_C_SOURCE 200809L

#include "push.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "configuration.h"
#include "cws/cws.h"
#include "integrations/fcm/fcm.h"
#include "integrations/push/push_outbox_domain.h"
#include "api/v1/routes/users/domain/users_device_domain.h"
#include "integrations/telegram/telegram_json.h"

typedef struct {
    const char* type;
    const char* priority;
    int ttl;
} push_catalog_t;

static const push_catalog_t g_catalog[] = {
    { "call.incoming", "high", 60 },
    { "call.cancel",   "high", 60 },
    { "call.missed",   "normal", 3600 },
    { "call.ended",    "normal", 600 },
    { "message",       "normal", 86400 },
    { "message.silent","normal", 86400 },
    { "message.summary","low", 86400 },
    { "message.read",  "normal", 600 },
    { "message.deleted","normal", 600 },
    { "sync.inbox",    "normal", 86400 },
};

static int g_running = 0;
static int g_started = 0;
static pthread_t g_thread;
static int g_enabled = 0;


static int cfg_int(const char* key, int def) {
    const char* v = cfg_getenv(key);
    if (!v || !*v) return def;
    return atoi(v);
}


static const push_catalog_t* catalog_for(const char* type) {
    for (size_t i = 0; i < sizeof(g_catalog) / sizeof(g_catalog[0]); i++)
        if (type && strcmp(type, g_catalog[i].type) == 0)
            return &g_catalog[i];
    return NULL;
}

static int ack_delay_s(void) {
    const char* v = cfg_getenv("FCM_ACK_TIMEOUT_MS");
    int ms = (v && *v) ? atoi(v) : 2000;
    if (ms < 500) ms = 500;
    if (ms > 10000) ms = 10000;
    return (ms + 999) / 1000;
}

static void json_escape(char* out, size_t cap, const char* s) {
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)(s ? s : "");
         *p && o + 2 < cap; p++) {
        switch (*p) {
            case '"': out[o++] = '\\'; out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
            case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
            case '\t': out[o++] = '\\'; out[o++] = 't'; break;
            default:
                if (*p < 0x20) {
                    int n = snprintf(out + o, cap - o, "\\u%04x", *p);
                    if (n < 0 || (size_t)n >= cap - o) return;
                    o += (size_t)n;
                } else {
                    out[o++] = (char)*p;
                }
                break;
        }
    }
    out[o] = '\0';
}

int push_notify_user(int64_t usu_id, const char* event_key,
                     const char* type, const char* payload_json,
                     const char* collapse_key, int delay_s) {
    if (!g_enabled || !fcm_enabled() || !type || !payload_json)
        return 0;
    const push_catalog_t* cat = catalog_for(type);
    const char* priority = cat ? cat->priority : "normal";
    int ttl = cat ? cat->ttl : 86400;

    sql_result_t* devs = users_devices_push_tokens(usu_id);
    if (!devs) {
        cws_log_warn("push: type=%s event=%s sin consulta de tokens",
                     type, event_key ? event_key : "");
        return 0;
    }
    int max_dev = cfg_int("FCM_MAX_DEVICES_PER_USER", 10);
    if (max_dev < 1) max_dev = 1;
    if (max_dev > 50) max_dev = 50;
    int sent = 0;
    for (size_t i = 0; i < devs->nrows && sent < max_dev; i++) {
        const char* device = NULL;
        const char* token = NULL;
        for (size_t c = 0; c < devs->ncols; c++) {
            if (devs->columns[c].name &&
                strcmp(devs->columns[c].name, "usd_device_id") == 0) {
                const sql_value_t* v = &devs->rows[i].cols[c];
                if (!v->is_null && v->type == SQL_COL_STRING)
                    device = v->val.as_string;
            } else if (devs->columns[c].name &&
                       strcmp(devs->columns[c].name, "usd_push_token") == 0) {
                const sql_value_t* v = &devs->rows[i].cols[c];
                if (!v->is_null && v->type == SQL_COL_STRING)
                    token = v->val.as_string;
            }
        }
        if (!device || !*device || !token || !*token) continue;
        if (push_outbox_enqueue(usu_id, device, token, type, payload_json,
                                collapse_key, priority, ttl, delay_s,
                                event_key))
            sent++;
    }
    size_t ndev = devs->nrows;
    sql_result_free(devs);
    cws_log_info("push: type=%s event=%s devices=%zu enqueued=%d delay=%ds",
                 type, event_key ? event_key : "", ndev, sent, delay_s);
    return sent;
}

static void sleep_seconds(int s) {
    struct timespec ts = { s, 0 };
    nanosleep(&ts, NULL);
}

static void* worker(void* arg) {
    (void)arg;
    while (g_running) {
        push_outbox_item_t items[20];
        int n = push_outbox_claim(20, items, 20);
        if (n > 0)
            cws_log_info("push: worker claim=%d", n);
        for (int i = 0; i < n; i++) {
            char err[384];
            int rc = fcm_send(items[i].token, items[i].payload,
                              items[i].priority, items[i].ttl,
                              items[i].collapse, err, sizeof(err));
            if (rc == FCM_SENT) {
                push_outbox_mark(items[i].id, 2, NULL, 0);
                cws_log_info("push: sent type=%s device=%s",
                             items[i].type, items[i].device_id);
            } else if (rc == FCM_INVALID_TOKEN) {
                push_outbox_mark(items[i].id, 4, err, 0);
                users_devices_deactivate(items[i].usu_id,
                                         items[i].device_id);
                cws_log_warn("push: token inválido device=%s; desactivado",
                             items[i].device_id);
            } else {
                int attempts = items[i].attempts + 1;
                if (attempts >= 5) {
                    push_outbox_mark(items[i].id, 4, err, 0);
                    cws_log_warn("push: dead type=%s device=%s error=%s",
                                 items[i].type, items[i].device_id, err);
                } else {
                    int retry = 15 * (1 << (attempts - 1));
                    if (retry > 300) retry = 300;
                    push_outbox_mark(items[i].id, 3, err, retry);
                    cws_log_warn("push: retry type=%s device=%s attempt=%d error=%s",
                                 items[i].type, items[i].device_id,
                                 attempts, err);
                }
            }
        }
        if (n == 0) sleep_seconds(2);
        else sleep_seconds(1);
    }
    return NULL;
}

int push_init(void) {
    push_outbox_domain_init();
    fcm_init();
    if (!fcm_enabled()) {
        g_enabled = 0;
        fprintf(stderr, "push: deshabilitado (FCM no configurado)\n");
        return 0;
    }
    g_enabled = 1;
    g_running = 1;
    if (pthread_create(&g_thread, NULL, worker, NULL) != 0) {
        g_enabled = 0;
        g_running = 0;
        fprintf(stderr, "push: no se pudo crear worker\n");
        return 0;
    }
    g_started = 1;
    fprintf(stderr, "push: worker activo\n");
    return 1;
}

void push_shutdown(void) {
    g_running = 0;
    if (g_started) {
        pthread_join(g_thread, NULL);
        g_started = 0;
    }
    fcm_shutdown();
    push_outbox_domain_shutdown();
    g_enabled = 0;
}

int push_enabled(void) {
    return g_enabled;
}

int push_notify_call(int64_t usu_id, const char* call_id,
                     const char* from_name, const char* from_public,
                     const char* tipo, int e2ee,
                     const char* event_key, const char* type) {
    if (!call_id) return 0;
    char n[128], p[64], c[80], e[80], t[32];
    json_escape(n, sizeof(n), from_name ? from_name : "");
    json_escape(p, sizeof(p), from_public ? from_public : "");
    json_escape(c, sizeof(c), call_id);
    json_escape(t, sizeof(t), tipo ? tipo : "audio");
    snprintf(e, sizeof(e), "cws://call/%s", c);
    char payload[1024];
    snprintf(payload, sizeof(payload),
             "{\"v\":\"1\",\"type\":\"%s\",\"event_id\":\"%s\","
             "\"ts\":\"%lld\",\"call_id\":\"%s\",\"from\":\"%s\","
             "\"from_public\":\"%s\",\"tipo\":\"%s\",\"e2ee\":\"%s\","
             "\"priority\":\"high\",\"ttl\":\"60\",\"silent\":\"false\","
             "\"deep_link\":\"%s\"}",
             type ? type : "call.incoming", event_key ? event_key : "",
             (long long)time(NULL), c, n, p, t, e2ee ? "true" : "false", e);
    char collapse[96];
    snprintf(collapse, sizeof(collapse), "call:%s", c);
    return push_notify_user(usu_id, event_key, type ? type : "call.incoming",
                            payload, collapse, ack_delay_s());
}

int push_notify_call_event(int64_t usu_id, const char* call_id,
                           const char* type) {
    if (!call_id || !type) return 0;
    char c[80], e[96];
    json_escape(c, sizeof(c), call_id);
    snprintf(e, sizeof(e), "cws://call/%s", c);
    char ev[160];
    snprintf(ev, sizeof(ev), "call:%s:%s", c, type);
    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"v\":\"1\",\"type\":\"%s\",\"event_id\":\"%s\","
             "\"ts\":\"%lld\",\"call_id\":\"%s\",\"deep_link\":\"%s\"}",
             type, ev, (long long)time(NULL), c, e);
    char collapse[96];
    snprintf(collapse, sizeof(collapse), "call:%s", c);
    return push_notify_user(usu_id, ev, type, payload, collapse, 0);
}

int push_notify_message(int64_t usu_id, const char* chat_public,
                        int64_t msg_id, const char* from_name,
                        const char* from_public, const char* body,
                        int silent) {
    if (!chat_public || !*chat_public) return 0;
    char chat[64], from[128], fromp[64], text[512];
    json_escape(chat, sizeof(chat), chat_public);
    json_escape(from, sizeof(from), from_name ? from_name : "");
    json_escape(fromp, sizeof(fromp), from_public ? from_public : "");
    json_escape(text, sizeof(text), body ? body : "");
    char ev[96], deep[96];
    snprintf(ev, sizeof(ev), "msg:%lld", (long long)msg_id);
    snprintf(deep, sizeof(deep), "cws://chat/%s", chat);
    char payload[1600];
    snprintf(payload, sizeof(payload),
             "{\"v\":\"1\",\"type\":\"%s\",\"event_id\":\"%s\","
             "\"ts\":\"%lld\",\"dedupe_key\":\"%s\",\"chat_id\":\"%s\","
             "\"msg_id\":\"%lld\",\"from\":\"%s\",\"from_public\":\"%s\","
             "\"title\":\"%s\",\"body\":\"%s\",\"silent\":\"%s\","
             "\"deep_link\":\"%s\"}",
             silent ? "message.silent" : "message", ev,
             (long long)time(NULL), ev, chat, (long long)msg_id,
             from, fromp, from, text, silent ? "true" : "false", deep);
    char collapse[96];
    snprintf(collapse, sizeof(collapse), "chat:%s", chat);
    return push_notify_user(usu_id, ev, silent ? "message.silent" : "message",
                            payload, collapse, ack_delay_s());
}
