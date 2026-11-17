#define _POSIX_C_SOURCE 200809L

#include "telegram.h"
#include "telegram_domain.h"
#include "telegram_json.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>

#include "configuration.h"
#include "cws/cws.h"
#include "ws_api/v1/ws_v1.h"

#define TG_MAX_RESPONSE (4u * 1024u * 1024u)
#define TG_DEFAULT_API  "https://api.telegram.org"

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} tg_buf_t;

static char g_token[256];
static char g_api_base[512];
static char g_username[128];
static int  g_poll_timeout = 20;
static int  g_enabled = 0;
static int  g_started = 0;
static volatile int g_running = 0;
static pthread_t g_thread;
static int g_curl_global = 0;
static int64_t g_offset = 0;

static const char* cfg_str(const char* key, const char* def) {
    const char* v = cfg_getenv(key);
    return (v && *v) ? v : def;
}

static int cfg_int(const char* key, int def) {
    const char* v = cfg_getenv(key);
    if (!v || !*v) return def;
    return atoi(v);
}

static void trim_trailing(char* s) {
    if (!s) return;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '/' || s[n - 1] == ' ' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = '\0';
}

static void copy_str(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static size_t write_cb(void* ptr, size_t size, size_t nmemb, void* ud) {
    tg_buf_t* b = (tg_buf_t*)ud;
    size_t n = size * nmemb;
    if (!b || n > TG_MAX_RESPONSE || b->len + n > TG_MAX_RESPONSE)
        return 0;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 4096;
        while (cap < b->len + n + 1) cap *= 2;
        char* p = (char*)realloc(b->data, cap);
        if (!p) return 0;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static char* http_post(const char* method, const char* body, size_t body_len,
                       long timeout_s, long* http_status) {
    if (!g_token[0] || !method) return NULL;
    if (http_status) *http_status = 0;

    char url[1024];
    int n = snprintf(url, sizeof(url), "%s/bot%s/%s", g_api_base, g_token,
                     method);
    if (n < 0 || (size_t)n >= sizeof(url)) return NULL;

    CURL* curl = curl_easy_init();
    if (!curl) return NULL;

    tg_buf_t resp = {0};
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_s > 0 ? timeout_s : 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "cws_sodastream/1.0");
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (http_status) *http_status = status;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        cws_log_warn("telegram: %s falló: %s", method, curl_easy_strerror(rc));
        free(resp.data);
        return NULL;
    }
    if (status < 200 || status >= 300) {
        cws_log_warn("telegram: %s HTTP %ld", method, status);
        /* Mantiene el cuerpo para que el llamador lea la descripción. */
    }
    if (!resp.data) resp.data = strdup("");
    return resp.data;
}

static void json_escape_into(tg_buf_t* b, const char* s) {
    if (!s) s = "";
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        char tmp[8];
        const char* add = NULL;
        switch (*p) {
            case '"': add = "\\\""; break;
            case '\\': add = "\\\\"; break;
            case '\n': add = "\\n"; break;
            case '\r': add = "\\r"; break;
            case '\t': add = "\\t"; break;
            default:
                if (*p < 0x20) {
                    snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                    add = tmp;
                }
                break;
        }
        if (add) {
            size_t an = strlen(add);
            if (b->len + an + 1 > b->cap) {
                size_t cap = b->cap ? b->cap * 2 : 512;
                while (cap < b->len + an + 1) cap *= 2;
                char* np = (char*)realloc(b->data, cap);
                if (!np) return;
                b->data = np;
                b->cap = cap;
            }
            memcpy(b->data + b->len, add, an);
            b->len += an;
        } else {
            if (b->len + 2 > b->cap) {
                size_t cap = b->cap ? b->cap * 2 : 512;
                char* np = (char*)realloc(b->data, cap);
                if (!np) return;
                b->data = np;
                b->cap = cap;
            }
            b->data[b->len++] = (char)*p;
        }
    }
    if (b->data) b->data[b->len] = '\0';
}

int telegram_send_message(int64_t chat_id, const char* text) {
    if (!g_enabled || chat_id == 0 || !text) return 0;

    tg_buf_t body = {0};
    char head[128];
    int n = snprintf(head, sizeof(head),
                     "{\"chat_id\":%lld,\"text\":\"", (long long)chat_id);
    if (n < 0) return 0;
    body.data = (char*)calloc(1, 512);
    if (!body.data) return 0;
    body.cap = 512;
    memcpy(body.data, head, (size_t)n);
    body.len = (size_t)n;
    json_escape_into(&body, text);
    const char* tail = "\",\"disable_web_page_preview\":true}";
    size_t tl = strlen(tail);
    if (body.len + tl + 1 > body.cap) {
        size_t cap = body.cap * 2;
        while (cap < body.len + tl + 1) cap *= 2;
        char* np = (char*)realloc(body.data, cap);
        if (!np) { free(body.data); return 0; }
        body.data = np;
        body.cap = cap;
    }
    memcpy(body.data + body.len, tail, tl + 1);
    body.len += tl;

    char* resp = http_post("sendMessage", body.data, body.len, 15, NULL);
    free(body.data);
    if (!resp) return 0;

    int ok = 0;
    tj_value_t* root = tj_parse(resp, strlen(resp));
    if (root) {
        ok = tj_bool(tj_obj_get(root, "ok"));
        if (!ok) {
            const char* desc = tj_str(tj_obj_get(root, "description"));
            if (desc) cws_log_warn("telegram: sendMessage: %s", desc);
        }
        tj_free(root);
    }
    free(resp);
    return ok;
}

int telegram_notify_call(int64_t usu_id, const char* from_name,
                         const char* call_id, const char* tipo) {
    if (!g_enabled || !call_id) return 0;
    telegram_link_t link;
    memset(&link, 0, sizeof(link));
    if (!telegram_domain_get_link(usu_id, &link) || link.chat_id == 0)
        return 0;

    const char* uri = cfg_str("TELEGRAM_CALL_URI", "cws");
    char text[640];
    snprintf(text, sizeof(text),
             "Llamada de %s (%s)\nAbre en la app: %s://call/%s",
             (from_name && *from_name) ? from_name : "un contacto",
             (tipo && *tipo) ? tipo : "audio", uri, call_id);
    return telegram_send_message(link.chat_id, text);
}

static void buf_puts(tg_buf_t* b, const char* s) {
    if (!b || !s) return;
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char* np = (char*)realloc(b->data, cap);
        if (!np) return;
        b->data = np;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

int telegram_notify_bridge(int64_t usu_id, const char* from_public,
                           const char* from_name, const char* from_ip,
                           const char* call_id, const char* tipo,
                           int e2ee_v, const char* e2ee_algs,
                           int e2ee_confirm, int* ws_online) {
    int online = ws_v1_user_online(usu_id);
    if (ws_online) *ws_online = online;

    tg_buf_t body = {0};
    body.data = (char*)calloc(1, 256);
    if (body.data) {
        body.cap = 256;
        buf_puts(&body, "{\"call_id\":\"");
        json_escape_into(&body, call_id ? call_id : "");
        buf_puts(&body, "\",\"from_public\":\"");
        json_escape_into(&body, (from_public && *from_public) ? from_public : "");
        buf_puts(&body, "\",\"from\":\"");
        json_escape_into(&body, (from_name && *from_name) ? from_name : "");
        buf_puts(&body, "\",\"ip\":\"");
        json_escape_into(&body, (from_ip && *from_ip) ? from_ip : "");
        buf_puts(&body, "\",\"tipo\":\"");
        json_escape_into(&body, (tipo && *tipo) ? tipo : "audio");
        if (e2ee_v > 0) {
            buf_puts(&body, "\",\"e2ee\":{\"v\":");
            char vbuf[16];
            snprintf(vbuf, sizeof(vbuf), "%d", e2ee_v);
            buf_puts(&body, vbuf);
            buf_puts(&body, ",\"algs\":[\"");
            json_escape_into(&body, (e2ee_algs && *e2ee_algs) ? "gost" : "gost");
            buf_puts(&body, "\"],\"confirm\":");
            buf_puts(&body, e2ee_confirm ? "true" : "false");
            buf_puts(&body, "}");
        }
        buf_puts(&body, "}");
        ws_v1_emit_to_user(usu_id, "call.incoming", body.data, body.len);
        free(body.data);
    }

    int sent = 0;
    if (!online || telegram_notify_always())
        sent = telegram_notify_call(usu_id, from_name, call_id, tipo);
    return sent;
}

static void bot_reply(int64_t chat_id, const char* text) {
    if (chat_id && text) telegram_send_message(chat_id, text);
}

static char* command_arg(const char* text, const char* cmd) {
    if (!text || !cmd) return NULL;
    size_t cl = strlen(cmd);
    if (strncmp(text, cmd, cl) != 0) return NULL;
    const char* p = text + cl;
    if (*p == '@') {
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    while (*p == ' ' || *p == '\t') p++;
    while (*p && (p[strlen(p) - 1] == ' ' || p[strlen(p) - 1] == '\t'))
        ((char*)p)[strlen(p) - 1] = '\0';
    return (char*)p;
}

static int is_command(const char* text, const char* cmd) {
    if (!text || !cmd) return 0;
    size_t n = strlen(cmd);
    if (strncmp(text, cmd, n) != 0) return 0;
    return text[n] == '\0' || text[n] == ' ' || text[n] == '\t' ||
           text[n] == '@';
}

static void handle_message(const tj_value_t* msg) {
    const tj_value_t* chat = tj_obj_get(msg, "chat");
    const tj_value_t* from = tj_obj_get(msg, "from");
    const char* text = tj_str(tj_obj_get(msg, "text"));
    if (!chat || !text) return;

    int64_t chat_id = tj_int(tj_obj_get(chat, "id"));
    const char* chat_type = tj_str(tj_obj_get(chat, "type"));
    if (chat_id == 0) return;

    if (is_command(text, "/start")) {
        if (chat_type && strcmp(chat_type, "private") != 0) {
            bot_reply(chat_id, "Usa el chat privado con el bot para vincular.");
            return;
        }
        char* code = command_arg(text, "/start");
        if (!code || !*code) {
            bot_reply(chat_id,
                      "Abre el enlace de vinculación desde la app "
                      "(Ajustes > Telegram) para conectar tu cuenta.");
            return;
        }
        const char* username = tj_str(tj_obj_get(from, "username"));
        const char* first = tj_str(tj_obj_get(from, "first_name"));
        char name[TELEGRAM_TG_NAME_MAX];
        copy_str(name, sizeof(name), first ? first : username);
        int64_t uid = 0;
        if (telegram_domain_consume_code(code, chat_id, username, name, &uid)) {
            bot_reply(chat_id,
                      "Listo. Tu cuenta quedó vinculada a Telegram.\n"
                      "Cuando no tengas la app abierta, aquí llegará el aviso "
                      "de llamada con un enlace para abrirla.");
            cws_log_info("telegram: chat %lld vinculado (usu_id=%lld)",
                         (long long)chat_id, (long long)uid);
        } else {
            bot_reply(chat_id,
                      "Código inválido o expirado. Genera uno nuevo desde "
                      "la app (Ajustes > Telegram).");
        }
        return;
    }

    if (is_command(text, "/stop")) {
        telegram_domain_unlink_chat(chat_id);
        bot_reply(chat_id, "Vinculación eliminada.");
        return;
    }

    if (is_command(text, "/status")) {
        telegram_link_t link;
        memset(&link, 0, sizeof(link));
        if (telegram_domain_get_link_by_chat(chat_id, &link))
            bot_reply(chat_id, "Tu Telegram está vinculado a una cuenta.");
        else
            bot_reply(chat_id, "Tu Telegram no está vinculado.");
        return;
    }

    if (is_command(text, "/help") || text[0] == '/') {
        bot_reply(chat_id,
                  "Comandos:\n"
                  "/start <código> - vincular cuenta\n"
                  "/status - ver vínculo\n"
                  "/stop - desvincular\n"
                  "/help - esta ayuda");
    }
}

static void process_updates(const char* json, size_t len) {
    tj_value_t* root = tj_parse(json, len);
    if (!root) {
        cws_log_warn("telegram: getUpdates JSON inválido");
        return;
    }
    if (!tj_bool(tj_obj_get(root, "ok"))) {
        const char* desc = tj_str(tj_obj_get(root, "description"));
        if (desc) cws_log_warn("telegram: getUpdates: %s", desc);
        tj_free(root);
        return;
    }
    tj_value_t* result = tj_obj_get(root, "result");
    size_t n = tj_arr_len(result);
    for (size_t i = 0; i < n; i++) {
        tj_value_t* upd = tj_arr_get(result, i);
        int64_t update_id = tj_int(tj_obj_get(upd, "update_id"));
        if (update_id >= g_offset) g_offset = update_id + 1;
        tj_value_t* msg = tj_obj_get(upd, "message");
        if (msg) handle_message(msg);
    }
    tj_free(root);
}

static void* bot_thread(void* arg) {
    (void)arg;
    /* Si el bot tenía un webhook, getUpdates devuelve 409. */
    char* dw = http_post("deleteWebhook", "{}", 2, 10, NULL);
    free(dw);
    while (g_running) {
        char body[256];
        int n = snprintf(body, sizeof(body),
                         "{\"offset\":%lld,\"timeout\":%d,"
                         "\"allowed_updates\":[\"message\"]}",
                         (long long)g_offset, g_poll_timeout);
        if (n < 0 || (size_t)n >= sizeof(body)) break;

        long http = 0;
        char* resp = http_post("getUpdates", body, (size_t)n,
                               (long)g_poll_timeout + 10, &http);
        if (!resp) {
            if (!g_running) break;
            sleep(5);
            continue;
        }
        if (http >= 200 && http < 300) {
            process_updates(resp, strlen(resp));
            struct timespec ts = { 0, 200000000L };
            nanosleep(&ts, NULL);
        } else {
            sleep(5);
        }
        free(resp);
    }
    return NULL;
}

int telegram_init(void) {
    const char* token = cfg_getenv("TELEGRAM_BOT_TOKEN");
    if (!token || !*token) {
        fprintf(stderr, "telegram: deshabilitado (TELEGRAM_BOT_TOKEN vacío)\n");
        return 0;
    }
    copy_str(g_token, sizeof(g_token), token);
    copy_str(g_api_base, sizeof(g_api_base),
             cfg_str("TELEGRAM_API_BASE", TG_DEFAULT_API));
    trim_trailing(g_api_base);
    copy_str(g_username, sizeof(g_username),
             cfg_str("TELEGRAM_BOT_USERNAME", ""));
    g_poll_timeout = cfg_int("TELEGRAM_POLL_TIMEOUT", 20);
    if (g_poll_timeout < 1) g_poll_timeout = 1;
    if (g_poll_timeout > 50) g_poll_timeout = 50;

    if (!g_curl_global) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
            cws_log_error("telegram: curl_global_init falló");
            return 0;
        }
        g_curl_global = 1;
    }
    g_enabled = 1;
    g_running = 1;
    if (pthread_create(&g_thread, NULL, bot_thread, NULL) != 0) {
        cws_log_error("telegram: no se pudo crear el hilo del bot");
        g_enabled = 0;
        g_running = 0;
        return 0;
    }
    g_started = 1;
    fprintf(stderr, "telegram: bot activo (@%s), long polling %ds\n",
            g_username[0] ? g_username : "?", g_poll_timeout);
    return 1;
}

void telegram_shutdown(void) {
    if (!g_enabled && !g_started && !g_curl_global) return;
    g_running = 0;
    if (g_started) {
        pthread_join(g_thread, NULL);
        g_started = 0;
    }
    g_enabled = 0;
    if (g_curl_global) {
        curl_global_cleanup();
        g_curl_global = 0;
    }
}

int telegram_enabled(void) {
    return g_enabled;
}

const char* telegram_bot_username(void) {
    return g_username;
}

int telegram_link_ttl_min(void) {
    int n = cfg_int("TELEGRAM_LINK_TTL_MIN", 15);
    if (n < 1) n = 1;
    if (n > 1440) n = 1440;
    return n;
}

int telegram_notify_always(void) {
    const char* v = cfg_getenv("TELEGRAM_NOTIFY_ALWAYS");
    return v && *v && (strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0);
}
