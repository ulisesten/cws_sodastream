#define _POSIX_C_SOURCE 200809L

#include "external_routes.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <gost/gost.h>

#include "configuration.h"
#include "jwt.h"
#include "integrations/telegram/telegram.h"
#include "integrations/telegram/telegram_domain.h"
#include "api/v1/routes/users/domain/users_domain.h"

#define USU_ID_PUBLIC_LEN 12
#define HMAC_TOLERANCE_S  300

typedef struct nonce_entry {
    char   nonce[129];
    time_t exp;
} nonce_entry_t;

static nonce_entry_t g_nonces[64];
static size_t g_nonce_pos = 0;
static pthread_mutex_t g_nonce_lock = PTHREAD_MUTEX_INITIALIZER;

static void send_json(cws_response_t* res, int status, const char* body) {
    cws_response_status(res, status);
    cws_response_body(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

static size_t json_escape(char* out, size_t cap, const char* s) {
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
                    if (n < 0 || (size_t)n >= cap - o) return o;
                    o += (size_t)n;
                } else {
                    out[o++] = (char)*p;
                }
                break;
        }
    }
    out[o] = '\0';
    return o;
}

static void send_error(cws_response_t* res, int status, const char* msg) {
    char body[512];
    int n = snprintf(body, sizeof(body),
                     "{\"success\":false,\"error\":1,\"msg\":\"");
    size_t len = n > 0 ? (size_t)n : 0;
    len += json_escape(body + len, sizeof(body) - len - 8, msg);
    n = snprintf(body + len, sizeof(body) - len, "\"}");
    if (n < 0) len = 0; else len += (size_t)n;
    send_json(res, status, body);
}

static char* header_dup(const cws_request_t* req, const char* name) {
    if (!req || !name) return NULL;
    size_t nlen = strlen(name);
    for (size_t i = 0; i < req->headers_count; i++) {
        const cws_header_t* h = &req->headers[i];
        if (h->name_len == nlen &&
            strncasecmp(h->name, name, nlen) == 0) {
            char* out = (char*)malloc(h->value_len + 1);
            if (!out) return NULL;
            memcpy(out, h->value ? h->value : "", h->value_len);
            out[h->value_len] = '\0';
            return out;
        }
    }
    return NULL;
}

static int nonce_seen(const char* nonce, time_t now) {
    int seen = 0;
    pthread_mutex_lock(&g_nonce_lock);
    for (size_t i = 0; i < sizeof(g_nonces) / sizeof(g_nonces[0]); i++) {
        if (g_nonces[i].nonce[0] && g_nonces[i].exp >= now &&
            strcmp(g_nonces[i].nonce, nonce) == 0) {
            seen = 1;
            break;
        }
    }
    if (!seen) {
        snprintf(g_nonces[g_nonce_pos].nonce,
                 sizeof(g_nonces[g_nonce_pos].nonce), "%s", nonce);
        g_nonces[g_nonce_pos].exp = now + (2 * HMAC_TOLERANCE_S);
        g_nonce_pos = (g_nonce_pos + 1) %
                      (sizeof(g_nonces) / sizeof(g_nonces[0]));
    }
    pthread_mutex_unlock(&g_nonce_lock);
    return seen;
}

static void external_auth_mw(cws_request_t* req, cws_response_t* res,
                             cws_next_fn next) {
    const char* secret = cfg_getenv("EXTERNAL_API_SECRET");
    if (!secret || !*secret) {
        send_error(res, 503, "Integración externa no configurada");
        return;
    }

    char* ts = header_dup(req, "X-Integration-Timestamp");
    char* nonce = header_dup(req, "X-Integration-Nonce");
    char* sig = header_dup(req, "X-Integration-Signature");
    if (!ts || !nonce || !sig) {
        free(ts); free(nonce); free(sig);
        send_error(res, 401, "Faltan headers de integración");
        return;
    }

    time_t now = time(NULL);
    long long t = atoll(ts);
    if (t <= 0 || llabs((long long)now - t) > HMAC_TOLERANCE_S) {
        free(ts); free(nonce); free(sig);
        send_error(res, 401, "Timestamp inválido");
        return;
    }
    if (strlen(nonce) < 8 || strlen(nonce) > 128) {
        free(ts); free(nonce); free(sig);
        send_error(res, 401, "Nonce inválido");
        return;
    }
    if (strlen(sig) != 64) {
        free(ts); free(nonce); free(sig);
        send_error(res, 401, "Firma inválida");
        return;
    }
    if (nonce_seen(nonce, now)) {
        free(ts); free(nonce); free(sig);
        send_error(res, 401, "Nonce reutilizado");
        return;
    }

    const char* body = req->body ? req->body : "";
    size_t body_len = req->body_len;
    size_t msg_len = strlen(ts) + 1 + strlen(nonce) + 1 + body_len;
    char* msg = (char*)malloc(msg_len + 1);
    if (!msg) {
        free(ts); free(nonce); free(sig);
        send_error(res, 500, "Sin memoria");
        return;
    }
    int n = snprintf(msg, msg_len + 1, "%s\n%s\n", ts, nonce);
    if (n > 0) memcpy(msg + n, body, body_len);
    msg[msg_len] = '\0';

    uint8_t mac[32];
    char hex[65];
    static const char hx[] = "0123456789abcdef";
    int ok = 0;
    if (gost_hmac((const uint8_t*)secret, strlen(secret),
                  (const uint8_t*)msg, msg_len, GOST_HASH_256, mac) == GOST_OK) {
        for (int i = 0; i < 32; i++) {
            hex[i * 2] = hx[mac[i] >> 4];
            hex[i * 2 + 1] = hx[mac[i] & 0x0f];
        }
        hex[64] = '\0';
        ok = strcasecmp(hex, sig) == 0;
    }
    free(msg); free(ts); free(nonce); free(sig);
    if (!ok) {
        send_error(res, 401, "Firma inválida");
        return;
    }
    next(req, res);
}

static int valid_public_id(const char* s) {
    if (!s || strlen(s) != USU_ID_PUBLIC_LEN) return 0;
    for (const char* p = s; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')
            continue;
        return 0;
    }
    return 1;
}

static int valid_call_id(const char* s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n < 6 || n > 64) return 0;
    for (const char* p = s; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')
            continue;
        return 0;
    }
    return 1;
}

static int valid_tipo(const char* s) {
    return s && (strcmp(s, "audio") == 0 || strcmp(s, "video") == 0 ||
                 strcmp(s, "walkie") == 0);
}

static const sql_value_t* row_field(const sql_result_t* out, size_t row,
                                    const char* name) {
    if (!out || row >= out->nrows) return NULL;
    for (size_t c = 0; c < out->ncols; c++) {
        const char* cn = out->columns[c].name;
        if (cn && strcmp(cn, name) == 0) return &out->rows[row].cols[c];
    }
    return NULL;
}

static const char* row_str(const sql_result_t* out, size_t row,
                           const char* name) {
    const sql_value_t* v = row_field(out, row, name);
    if (!v || v->is_null || v->type != SQL_COL_STRING || !v->val.as_string)
        return NULL;
    return v->val.as_string;
}

static int64_t row_i64(const sql_result_t* out, size_t row, const char* name,
                       int64_t def) {
    const sql_value_t* v = row_field(out, row, name);
    if (!v || v->is_null || v->type != SQL_COL_INT) return def;
    return v->val.as_int;
}

static char* body_dup(const cws_request_t* req) {
    if (!req || !req->body || req->body_len == 0 || req->body_len > 8192)
        return NULL;
    char* b = (char*)malloc(req->body_len + 1);
    if (!b) return NULL;
    memcpy(b, req->body, req->body_len);
    b[req->body_len] = '\0';
    return b;
}

/* ------------------------------------------------------------------ */
/* POST /link  { usu_id_public, telefono?, ip?, id_externo? }          */
/* ------------------------------------------------------------------ */
static CWS_HANDLER(external_link_handler) {
    char* body = body_dup(req);
    char* public_id = NULL;
    char* telefono = NULL;
    char* ip = NULL;
    char* id_externo = NULL;
    if (body) {
        jwt_json_get_string(body, "usu_id_public", &public_id);
        jwt_json_get_string(body, "telefono", &telefono);
        jwt_json_get_string(body, "ip", &ip);
        jwt_json_get_string(body, "id_externo", &id_externo);
    }
    if (!valid_public_id(public_id)) {
        free(body); free(public_id); free(telefono); free(ip); free(id_externo);
        send_error(res, 400, "usu_id_public válido es requerido");
        return;
    }

    sql_result_t* dao = users_domain_by_public_id(public_id);
    int64_t uid = dao ? row_i64(dao, 0, "usu_id", 0) : 0;
    if (dao) sql_result_free(dao);
    if (uid == 0) {
        free(body); free(public_id); free(telefono); free(ip); free(id_externo);
        send_error(res, 404, "Usuario no encontrado");
        return;
    }

    int n = 0;
    if (telefono && *telefono && telegram_domain_external_link(uid, 1, telefono))
        n++;
    if (ip && *ip && telegram_domain_external_link(uid, 2, ip))
        n++;
    if (id_externo && *id_externo &&
        telegram_domain_external_link(uid, 3, id_externo))
        n++;

    free(body); free(public_id); free(telefono); free(ip); free(id_externo);
    if (n == 0) {
        send_error(res, 400, "Nada que mapear (telefono/ip/id_externo)");
        return;
    }
    send_json(res, 200,
              "{\"success\":true,\"error\":0,\"msg\":\"Mapeo guardado\"}");
}

/* ------------------------------------------------------------------ */
/* POST /notify { usu_id_public?|telefono?|ip?|id_externo?, ... }      */
/* ------------------------------------------------------------------ */
static CWS_HANDLER(external_notify_handler) {
    char* body = body_dup(req);
    char* public_id = NULL;
    char* telefono = NULL;
    char* ip = NULL;
    char* id_externo = NULL;
    char* call_id = NULL;
    char* tipo = NULL;
    char* nombre = NULL;
    if (body) {
        jwt_json_get_string(body, "usu_id_public", &public_id);
        jwt_json_get_string(body, "telefono", &telefono);
        jwt_json_get_string(body, "ip", &ip);
        jwt_json_get_string(body, "id_externo", &id_externo);
        jwt_json_get_string(body, "call_id", &call_id);
        jwt_json_get_string(body, "tipo", &tipo);
        jwt_json_get_string(body, "nombre", &nombre);
    }
    if (!valid_call_id(call_id) || !valid_tipo(tipo)) {
        free(body); free(public_id); free(telefono); free(ip);
        free(id_externo); free(call_id); free(tipo); free(nombre);
        send_error(res, 400, "call_id y tipo válidos son requeridos");
        return;
    }

    int64_t uid = 0;
    char callee_public[16] = "";
    char callee_name[128] = "";
    if (valid_public_id(public_id)) {
        sql_result_t* dao = users_domain_by_public_id(public_id);
        uid = dao ? row_i64(dao, 0, "usu_id", 0) : 0;
        if (dao) {
            const char* nm = row_str(dao, 0, "usu_nombre");
            if (nm) snprintf(callee_name, sizeof(callee_name), "%s", nm);
            sql_result_free(dao);
        }
        snprintf(callee_public, sizeof(callee_public), "%s", public_id);
    } else {
        int type = 0;
        const char* value = NULL;
        if (telefono && *telefono) { type = 1; value = telefono; }
        else if (ip && *ip) { type = 2; value = ip; }
        else if (id_externo && *id_externo) { type = 3; value = id_externo; }
        if (type)
            telegram_domain_external_lookup(type, value, &uid, callee_public,
                                            sizeof(callee_public), callee_name,
                                            sizeof(callee_name));
    }

    if (uid == 0) {
        free(body); free(public_id); free(telefono); free(ip);
        free(id_externo); free(call_id); free(tipo); free(nombre);
        send_error(res, 404, "Usuario no encontrado");
        return;
    }

    int ws_online = 0;
    int tg_sent = telegram_notify_bridge(uid, "", nombre, "", call_id, tipo,
                                         &ws_online);
    char name_esc[256];
    json_escape(name_esc, sizeof(name_esc), callee_name);
    char resp[512];
    snprintf(resp, sizeof(resp),
             "{\"success\":true,\"error\":0,\"data\":{"
             "\"callee\":\"%s\",\"ws_online\":%s,\"telegram_sent\":%s}}",
             name_esc, ws_online ? "true" : "false",
             tg_sent ? "true" : "false");
    free(body); free(public_id); free(telefono); free(ip);
    free(id_externo); free(call_id); free(tipo); free(nombre);
    send_json(res, 200, resp);
}

cws_router_t* external_routes(void) {
    cws_router_t* r = cws_router_new();
    if (!r) return NULL;
    cws_router_use(r, external_auth_mw);
    cws_router_add(r, CWS_M_POST, "/link", external_link_handler);
    cws_router_add(r, CWS_M_POST, "/notify", external_notify_handler);
    return r;
}
