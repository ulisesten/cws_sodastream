#define _POSIX_C_SOURCE 200809L

#include "telegram_routes.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "authorization_mobile.h"
#include "configuration.h"
#include "jwt.h"
#include "integrations/telegram/telegram.h"
#include "integrations/telegram/telegram_domain.h"
#include "integrations/telegram/telegram_calls.h"
#include "integrations/push/push.h"
#include "integrations/telegram/telegram_json.h"
#include "ws_api/v1/ws_v1.h"
#include "api/v1/routes/users/domain/users_domain.h"

#define USU_ID_PUBLIC_LEN 12

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

static void send_error_code(cws_response_t* res, int status,
                            const char* code, const char* msg) {
    char body[640];
    int n = snprintf(body, sizeof(body),
                     "{\"success\":false,\"error\":1,\"code\":\"");
    size_t len = n > 0 ? (size_t)n : 0;
    len += json_escape(body + len, sizeof(body) - len - 32, code);
    n = snprintf(body + len, sizeof(body) - len, "\",\"msg\":\"");
    if (n < 0) len = 0; else len += (size_t)n;
    len += json_escape(body + len, sizeof(body) - len - 8, msg);
    n = snprintf(body + len, sizeof(body) - len, "\"}");
    if (n < 0) len = 0; else len += (size_t)n;
    send_json(res, status, body);
}

static int cfg_bool(const char* key, int def) {
    const char* v = cfg_getenv(key);
    if (!v || !*v) return def;
    return !(strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 ||
             strcasecmp(v, "no") == 0);
}

typedef struct {
    int  present;
    int  v;
    int  confirm;
    int  has_gost;
    char algs[64];
} e2ee_cap_t;

static int parse_e2ee(const char* body, e2ee_cap_t* cap) {
    memset(cap, 0, sizeof(*cap));
    if (!body) return 0;
    tj_value_t* root = tj_parse(body, strlen(body));
    if (!root) return 0;
    tj_value_t* e = tj_obj_get(root, "e2ee");
    if (!e || tj_type(e) != TJ_OBJ) {
        tj_free(root);
        return 0;
    }
    cap->present = 1;
    cap->v = (int)tj_int(tj_obj_get(e, "v"));
    cap->confirm = tj_bool(tj_obj_get(e, "confirm"));
    tj_value_t* algs = tj_obj_get(e, "algs");
    size_t n = tj_arr_len(algs);
    for (size_t i = 0; i < n; i++) {
        const char* a = tj_str(tj_arr_get(algs, i));
        if (a && strcmp(a, "gost") == 0) {
            cap->has_gost = 1;
            snprintf(cap->algs, sizeof(cap->algs), "gost");
            break;
        }
    }
    tj_free(root);
    return 1;
}

static int e2ee_validate(const e2ee_cap_t* cap, cws_response_t* res) {
    int required = cfg_bool("CALL_E2EE_REQUIRED", 1);
    if (!cap->present) {
        if (required) {
            send_error_code(res, 426, "E2EE_UNSUPPORTED",
                            "La llamada exige E2EE GOST");
            return 0;
        }
        return 1;
    }
    if (cap->v != 1) {
        send_error_code(res, 426, "E2EE_VERSION_UNSUPPORTED",
                        "Versión E2EE no soportada");
        return 0;
    }
    if (!cap->has_gost) {
        send_error_code(res, 426, "E2EE_UNSUPPORTED",
                        "Se requiere el algoritmo gost");
        return 0;
    }
    return 1;
}

static int e2ee_json(char* out, size_t cap_size, const e2ee_cap_t* cap) {
    if (!cap || !cap->present || cap->v <= 0) {
        snprintf(out, cap_size, "null");
        return 0;
    }
    return snprintf(out, cap_size,
                    "{\"v\":%d,\"algs\":[\"%s\"],\"confirm\":%s}",
                    cap->v, cap->algs, cap->confirm ? "true" : "false");
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

static void copy_trim(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    while (*src == ' ' || *src == '\t') src++;
    size_t n = 0;
    while (src[n] && src[n] != ',' && src[n] != ' ' && src[n] != '\t' &&
           n + 1 < cap)
        n++;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* IP del cliente: body, X-Forwarded-For o X-Real-IP. */
static void request_ip(const cws_request_t* req, const char* body_ip,
                       char* out, size_t cap) {
    if (!out || cap == 0) return;
    /* Prioriza la IP que ve el proxy; el body es respaldo. */
    char* xff = header_dup(req, "X-Forwarded-For");
    if (xff && *xff) {
        copy_trim(out, cap, xff);
        free(xff);
        return;
    }
    free(xff);
    char* real = header_dup(req, "X-Real-IP");
    if (real && *real) {
        copy_trim(out, cap, real);
        free(real);
        return;
    }
    free(real);
    if (body_ip && *body_ip) {
        copy_trim(out, cap, body_ip);
        return;
    }
    out[0] = '\0';
}

static void emit_event(int64_t uid, const char* event, const char* payload) {
    if (uid && event && payload)
        ws_v1_emit_to_user(uid, event, payload, strlen(payload));
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

/* ------------------------------------------------------------------ */
/* POST /link                                                          */
/* ------------------------------------------------------------------ */
static CWS_HANDLER(telegram_link_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { send_error(res, 401, "No autorizado"); return; }

    if (!telegram_enabled()) {
        send_error(res, 503, "Bot de Telegram no configurado");
        return;
    }
    const char* bot = telegram_bot_username();
    if (!bot || !*bot) {
        send_error(res, 500, "TELEGRAM_BOT_USERNAME no configurado");
        return;
    }

    char* code = jwt_core_nanoid(32);
    if (!code) { send_error(res, 500, "No se pudo generar el código"); return; }

    int ttl = telegram_link_ttl_min();
    if (!telegram_domain_create_code(u->usu_id, code, ttl)) {
        free(code);
        send_error(res, 500, "No se pudo guardar el código");
        return;
    }

    /* t.me no necesita @ delante. */
    const char* user = bot;
    if (*user == '@') user++;
    char body[512];
    snprintf(body, sizeof(body),
             "{\"success\":true,\"error\":0,\"data\":{"
             "\"code\":\"%s\",\"url\":\"https://t.me/%s?start=%s\","
             "\"expires_in\":%d}}",
             code, user, code, ttl * 60);
    free(code);
    send_json(res, 200, body);
}

/* ------------------------------------------------------------------ */
/* GET /status                                                         */
/* ------------------------------------------------------------------ */
static CWS_HANDLER(telegram_status_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { send_error(res, 401, "No autorizado"); return; }

    telegram_link_t link;
    memset(&link, 0, sizeof(link));
    int linked = telegram_domain_get_link(u->usu_id, &link) &&
                 link.chat_id != 0;
    char uname[TELEGRAM_TG_USER_MAX], name[TELEGRAM_TG_NAME_MAX];
    json_escape(uname, sizeof(uname), linked ? link.username : "");
    json_escape(name, sizeof(name), linked ? link.name : "");
    char body[512];
    snprintf(body, sizeof(body),
             "{\"success\":true,\"error\":0,\"data\":{"
             "\"linked\":%s,\"telegram_username\":\"%s\","
             "\"telegram_name\":\"%s\"}}",
             linked ? "true" : "false", uname, name);
    send_json(res, 200, body);
}

/* ------------------------------------------------------------------ */
/* POST /unlink                                                        */
/* ------------------------------------------------------------------ */
static CWS_HANDLER(telegram_unlink_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { send_error(res, 401, "No autorizado"); return; }
    if (!telegram_domain_unlink_user(u->usu_id)) {
        send_error(res, 500, "No se pudo desvincular");
        return;
    }
    send_json(res, 200,
              "{\"success\":true,\"error\":0,\"msg\":\"Telegram desvinculado\"}");
}

/* ------------------------------------------------------------------ */
/* POST /notify                                                        */
/* ------------------------------------------------------------------ */
static CWS_HANDLER(telegram_notify_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { send_error(res, 401, "No autorizado"); return; }

    char* body = body_dup(req);
    char* public_id = NULL;
    char* call_id = NULL;
    char* tipo = NULL;
    char* body_ip = NULL;
    char* telefono = NULL;
    if (body) {
        jwt_json_get_string(body, "usu_id_public", &public_id);
        jwt_json_get_string(body, "call_id", &call_id);
        jwt_json_get_string(body, "tipo", &tipo);
        jwt_json_get_string(body, "ip", &body_ip);
        jwt_json_get_string(body, "telefono", &telefono);
    }
    if (!valid_public_id(public_id) || !valid_tipo(tipo)) {
        free(body); free(public_id); free(call_id); free(tipo);
        free(body_ip); free(telefono);
        send_error(res, 400, "usu_id_public y tipo válidos son requeridos");
        return;
    }
    e2ee_cap_t cap;
    parse_e2ee(body, &cap);
    if (!e2ee_validate(&cap, res)) {
        free(body); free(public_id); free(call_id); free(tipo);
        free(body_ip); free(telefono);
        return;
    }

    int server_id = cfg_bool("CALL_ID_SERVER", 1);
    if (server_id) {
        free(call_id);
        call_id = NULL;
        for (int i = 0; i < 8; i++) {
            char* id = jwt_core_nanoid(32);
            if (!id) break;
            telegram_call_t ex;
            memset(&ex, 0, sizeof(ex));
            if (!telegram_calls_get(id, &ex)) { call_id = id; break; }
            free(id);
        }
        if (!call_id) {
            free(body); free(public_id); free(tipo);
            free(body_ip); free(telefono);
            send_error(res, 500, "No se pudo generar call_id");
            return;
        }
    } else if (!valid_call_id(call_id)) {
        free(body); free(public_id); free(call_id); free(tipo);
        free(body_ip); free(telefono);
        send_error(res, 400, "call_id inválido");
        return;
    }

    sql_result_t* dao = users_domain_by_public_id(public_id);
    int64_t callee = dao ? row_i64(dao, 0, "usu_id", 0) : 0;
    const char* callee_name = dao ? row_str(dao, 0, "usu_nombre") : NULL;
    char callee_name_copy[TC_NAME_MAX];
    callee_name_copy[0] = '\0';
    if (callee_name)
        snprintf(callee_name_copy, sizeof(callee_name_copy), "%s", callee_name);
    if (dao) sql_result_free(dao);

    if (callee == 0) {
        free(body); free(public_id); free(call_id); free(tipo);
        free(body_ip); free(telefono);
        send_error(res, 404, "Usuario no encontrado");
        return;
    }
    if (callee == u->usu_id) {
        free(body); free(public_id); free(call_id); free(tipo);
        free(body_ip); free(telefono);
        send_error(res, 400, "No se puede llamar a uno mismo");
        return;
    }

    if (!server_id) {
        telegram_call_t ex;
        memset(&ex, 0, sizeof(ex));
        if (telegram_calls_get(call_id, &ex) &&
            (ex.caller_uid != u->usu_id || ex.callee_uid != callee)) {
            free(body); free(public_id); free(call_id); free(tipo);
            free(body_ip); free(telefono);
            send_error_code(res, 409, "CALL_ID_CONFLICT",
                            "call_id ya pertenece a otra llamada");
            return;
        }
    }

    char caller_ip[TC_IP_MAX];
    request_ip(req, body_ip, caller_ip, sizeof(caller_ip));
    char caller_public[TC_PUBLIC_MAX] = "";
    telegram_domain_user_public(u->usu_id, caller_public,
                                sizeof(caller_public), NULL, 0);

    int e2ee_v = cap.present ? cap.v : 0;
    const char* e2ee_algs = cap.present ? cap.algs : "";
    int e2ee_confirm = cap.present ? cap.confirm : 0;

    telegram_calls_upsert(call_id, u->usu_id, callee, caller_public, public_id,
                          u->usu_nombre, callee_name_copy, tipo, caller_ip,
                          telefono, e2ee_v, e2ee_algs, e2ee_confirm);
    if (caller_ip[0])
        telegram_domain_external_link(u->usu_id, 2, caller_ip);
    if (telefono && *telefono)
        telegram_domain_external_link(u->usu_id, 1, telefono);

    int ws_online = 0;
    int tg_sent = telegram_notify_bridge(callee, caller_public, u->usu_nombre,
                                         caller_ip, call_id, tipo, e2ee_v,
                                         e2ee_algs, e2ee_confirm, &ws_online);

    char callee_esc[256];
    json_escape(callee_esc, sizeof(callee_esc), callee_name_copy);
    char resp[768];
    snprintf(resp, sizeof(resp),
             "{\"success\":true,\"error\":0,\"data\":{"
             "\"call_id\":\"%s\",\"ws_online\":%s,\"telegram_sent\":%s,"
             "\"callee\":\"%s\",\"e2ee\":%s}}",
             call_id, ws_online ? "true" : "false",
             tg_sent ? "true" : "false", callee_esc,
             cap.present ? "true" : "false");
    free(body); free(public_id); free(call_id); free(tipo);
    free(body_ip); free(telefono);
    send_json(res, 200, resp);
}

/* ------------------------------------------------------------------ */
/* POST /accept  (receptor)                                            */
/* ------------------------------------------------------------------ */
static CWS_HANDLER(telegram_accept_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { send_error(res, 401, "No autorizado"); return; }

    char* body = body_dup(req);
    char* call_id = NULL;
    char* body_ip = NULL;
    char* telefono = NULL;
    if (body) {
        jwt_json_get_string(body, "call_id", &call_id);
        jwt_json_get_string(body, "ip", &body_ip);
        jwt_json_get_string(body, "telefono", &telefono);
    }
    if (!valid_call_id(call_id)) {
        free(body); free(call_id); free(body_ip); free(telefono);
        send_error(res, 400, "call_id requerido");
        return;
    }
    e2ee_cap_t cap;
    parse_e2ee(body, &cap);
    if (!e2ee_validate(&cap, res)) {
        free(body); free(call_id); free(body_ip); free(telefono);
        return;
    }

    char callee_ip[TC_IP_MAX];
    request_ip(req, body_ip, callee_ip, sizeof(callee_ip));
    int e2ee_v = cap.present ? cap.v : 0;
    const char* e2ee_algs = cap.present ? cap.algs : "";
    int e2ee_confirm = cap.present ? cap.confirm : 0;

    telegram_call_t call;
    memset(&call, 0, sizeof(call));
    if (!telegram_calls_accept(call_id, u->usu_id, callee_ip, telefono,
                               e2ee_v, e2ee_algs, e2ee_confirm, &call)) {
        free(body); free(call_id); free(body_ip); free(telefono);
        send_error(res, 409, "Llamada no encontrada, ya aceptada o no es tuya");
        return;
    }
    if (callee_ip[0])
        telegram_domain_external_link(u->usu_id, 2, callee_ip);
    if (telefono && *telefono)
        telegram_domain_external_link(u->usu_id, 1, telefono);

    char pub[TC_PUBLIC_MAX], name[TC_NAME_MAX], ip[TC_IP_MAX], tipo[TC_TIPO_MAX];
    char e2ee_evt[128];
    json_escape(pub, sizeof(pub), call.callee_public);
    json_escape(name, sizeof(name), call.callee_name);
    json_escape(ip, sizeof(ip), callee_ip);
    json_escape(tipo, sizeof(tipo), call.tipo);
    e2ee_json(e2ee_evt, sizeof(e2ee_evt), &cap);
    char evt[1024];
    snprintf(evt, sizeof(evt),
             "{\"call_id\":\"%s\",\"estado\":\"accepted\","
             "\"from_public\":\"%s\",\"from\":\"%s\",\"tipo\":\"%s\","
             "\"ip\":\"%s\",\"e2ee\":%s}",
             call_id, pub, name, tipo, ip, e2ee_evt);
    emit_event(call.caller_uid, "call.accepted", evt);

    char caller_pub[TC_PUBLIC_MAX], caller_name[TC_NAME_MAX];
    json_escape(caller_pub, sizeof(caller_pub), call.caller_public);
    json_escape(caller_name, sizeof(caller_name), call.caller_name);
    char resp[768];
    snprintf(resp, sizeof(resp),
             "{\"success\":true,\"error\":0,\"data\":{"
             "\"estado\":\"accepted\",\"caller_public\":\"%s\","
             "\"caller_name\":\"%s\",\"tipo\":\"%s\",\"e2ee\":%s}}",
             caller_pub, caller_name, tipo, cap.present ? "true" : "false");
    free(body); free(call_id); free(body_ip); free(telefono);
    send_json(res, 200, resp);
}

/* ------------------------------------------------------------------ */
/* POST /reject  (receptor)                                            */
/* ------------------------------------------------------------------ */
static CWS_HANDLER(telegram_reject_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { send_error(res, 401, "No autorizado"); return; }
    char* body = body_dup(req);
    char* call_id = NULL;
    if (body) jwt_json_get_string(body, "call_id", &call_id);
    if (!valid_call_id(call_id)) {
        free(body); free(call_id);
        send_error(res, 400, "call_id requerido");
        return;
    }
    telegram_call_t call;
    memset(&call, 0, sizeof(call));
    if (!telegram_calls_reject(call_id, u->usu_id, &call)) {
        free(body); free(call_id);
        send_error(res, 409, "Llamada no encontrada o no es tuya");
        return;
    }
    char evt[256];
    snprintf(evt, sizeof(evt),
             "{\"call_id\":\"%s\",\"estado\":\"rejected\"}", call_id);
    emit_event(call.caller_uid, "call.rejected", evt);
    {
        int p = push_notify_call_event(call.caller_uid, call_id, "call.cancel");
        cws_log_info("calls: call.cancel push=%d", p);
    }
    free(body); free(call_id);
    send_json(res, 200,
              "{\"success\":true,\"error\":0,\"data\":{\"estado\":\"rejected\"}}");
}

/* ------------------------------------------------------------------ */
/* POST /end  (cualquiera de los dos)                                  */
/* ------------------------------------------------------------------ */
static CWS_HANDLER(telegram_end_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { send_error(res, 401, "No autorizado"); return; }
    char* body = body_dup(req);
    char* call_id = NULL;
    if (body) jwt_json_get_string(body, "call_id", &call_id);
    if (!valid_call_id(call_id)) {
        free(body); free(call_id);
        send_error(res, 400, "call_id requerido");
        return;
    }
    telegram_call_t call;
    memset(&call, 0, sizeof(call));
    if (!telegram_calls_end(call_id, u->usu_id, &call)) {
        free(body); free(call_id);
        send_error(res, 409, "Llamada no encontrada o no es tuya");
        return;
    }
    int64_t other = (call.caller_uid == u->usu_id) ? call.callee_uid
                                                   : call.caller_uid;
    char evt[256];
    snprintf(evt, sizeof(evt),
             "{\"call_id\":\"%s\",\"estado\":\"ended\"}", call_id);
    emit_event(other, "call.ended", evt);
    {
        int p = push_notify_call_event(other, call_id, "call.ended");
        cws_log_info("calls: call.ended push=%d", p);
    }
    free(body); free(call_id);
    send_json(res, 200,
              "{\"success\":true,\"error\":0,\"data\":{\"estado\":\"ended\"}}");
}

/* ------------------------------------------------------------------ */
/* GET /pending                                                        */
/* ------------------------------------------------------------------ */
static CWS_HANDLER(telegram_pending_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { send_error(res, 401, "No autorizado"); return; }

    telegram_call_t calls[32];
    int n = telegram_calls_pending(u->usu_id, calls, 32);
    char body[16384];
    size_t o = 0;
    o += (size_t)snprintf(body + o, sizeof(body) - o,
                          "{\"success\":true,\"error\":0,\"data\":{\"calls\":[");
    for (int i = 0; i < n; i++) {
        const telegram_call_t* c = &calls[i];
        int is_callee = c->callee_uid == u->usu_id;
        const char* from_pub = is_callee ? c->caller_public : c->callee_public;
        const char* from_name = is_callee ? c->caller_name : c->callee_name;
        char pub[TC_PUBLIC_MAX], name[TC_NAME_MAX], tipo[TC_TIPO_MAX];
        char ip[TC_IP_MAX];
        json_escape(pub, sizeof(pub), from_pub);
        json_escape(name, sizeof(name), from_name);
        json_escape(tipo, sizeof(tipo), c->tipo);
        json_escape(ip, sizeof(ip), is_callee ? c->caller_ip : c->callee_ip);
        if (o > sizeof(body) - 640) break;
        o += (size_t)snprintf(body + o, sizeof(body) - o,
                 "%s{\"call_id\":\"%s\",\"tipo\":\"%s\",\"estado\":\"%s\","
                 "\"from_public\":\"%s\",\"from\":\"%s\",\"ip\":\"%s\","
                 "\"ts\":%lld}",
                 i ? "," : "", c->call_id, tipo,
                 c->state == 0 ? "ringing" : "accepted",
                 pub, name, ip, (long long)c->created);
    }
    snprintf(body + o, sizeof(body) - o, "]}}");
    send_json(res, 200, body);
}

cws_router_t* telegram_routes(void) {
    cws_router_t* r = cws_router_new();
    if (!r) return NULL;
    cws_router_use(r, cws_mw_authorization_mobile_verify);
    cws_router_add(r, CWS_M_POST, "/link", telegram_link_handler);
    cws_router_add(r, CWS_M_GET, "/status", telegram_status_handler);
    cws_router_add(r, CWS_M_POST, "/unlink", telegram_unlink_handler);
    cws_router_add(r, CWS_M_POST, "/notify", telegram_notify_handler);
    cws_router_add(r, CWS_M_POST, "/accept", telegram_accept_handler);
    cws_router_add(r, CWS_M_POST, "/reject", telegram_reject_handler);
    cws_router_add(r, CWS_M_POST, "/end", telegram_end_handler);
    cws_router_add(r, CWS_M_GET, "/pending", telegram_pending_handler);
    return r;
}
