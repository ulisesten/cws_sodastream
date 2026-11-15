/*
 * messaging_routes.c — mensajería instantánea (chat 1:1).
 *
 * Todas las rutas exigen access token móvil (Bearer):
 *
 *   POST /setup                              habilita el chat y genera el id
 *                                            público (nanoid 12)
 *   GET  /users/:public_id                   busca un usuario por id público
 *   GET  /conversations/:public_id/messages  historial paginado
 *   POST /conversations/:public_id/messages  envía un mensaje (HTTP) y lo
 *                                            empuja por WS (`dm`)
 *   POST /conversations/:public_id/read      marca mensajes como leídos
 *
 * `usu_id` es interno y nunca se devuelve; el cliente solo usa
 * `usu_id_public` y `msg_id`.
 */

#define _POSIX_C_SOURCE 200809L

#include "messaging_routes.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "authorization_mobile.h"
#include "jwt.h"
#include "ws_api/v1/ws_v1.h"
#include "../users/domain/users_domain.h"
#include "domain/messaging_domain.h"

/* cat_usuarios.usu_id_public es char(12). */
#define USU_ID_PUBLIC_LEN 12
#define WS_V1_PATH        "/ws/v1"
/* Límite del WS (bytes UTF-8). */
#define WS_MAX_TEXT_BYTES 4096
#define CHAT_DEFAULT_LIMIT 30
#define CHAT_MAX_LIMIT     100

/* ------------------------------------------------------------------ */
/* helpers de respuesta / parámetros                                   */
/* ------------------------------------------------------------------ */

static void send_json(cws_response_t* res, int status, const char* body) {
    cws_response_status(res, status);
    cws_response_body(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

/* Copia `s` a `out` con escapes JSON mínimos (", \ y controles). */
static size_t json_escape_to(char* out, size_t cap, const char* s) {
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)(s ? s : "");
         *p && o + 2 < cap; p++) {
        switch (*p) {
            case '"':  out[o++] = '\\'; out[o++] = '"'; break;
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

/* {"success":false,"error":1,"msg":"..."} */
static void send_error_msg(cws_response_t* res, int status, const char* msg) {
    char body[512];
    int n = snprintf(body, sizeof(body),
                     "{\"success\":false,\"error\":1,\"msg\":\"");
    size_t len = n > 0 ? (size_t)n : 0;
    len += json_escape_to(body + len, sizeof(body) - len - 8, msg);
    n = snprintf(body + len, sizeof(body) - len, "\"}");
    if (n < 0) len = 0;
    else len += (size_t)n;
    cws_response_status(res, status);
    cws_response_body(res, body, len, CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

/* Copia NUL-terminada de un path-param. Liberar con free. */
static char* param_dup(const cws_request_t* req, const char* name) {
    if (!req || !name) return NULL;
    size_t kl = strlen(name);
    for (size_t i = 0; i < req->path_params_count; i++) {
        const cws_query_kv_t* kv = &req->path_params[i];
        if (kv->key_len != kl || memcmp(kv->key, name, kl) != 0) continue;
        char* out = (char*)malloc(kv->value_len + 1);
        if (!out) return NULL;
        memcpy(out, kv->value, kv->value_len);
        out[kv->value_len] = '\0';
        return out;
    }
    return NULL;
}

/* Copia NUL-terminada de un query-param. Liberar con free. */
static char* query_dup(const cws_request_t* req, const char* name) {
    if (!req || !name) return NULL;
    size_t kl = strlen(name);
    for (size_t i = 0; i < req->query_params_count; i++) {
        const cws_query_kv_t* kv = &req->query_params[i];
        if (kv->key_len != kl || memcmp(kv->key, name, kl) != 0) continue;
        char* out = (char*)malloc(kv->value_len + 1);
        if (!out) return NULL;
        memcpy(out, kv->value, kv->value_len);
        out[kv->value_len] = '\0';
        return out;
    }
    return NULL;
}

/* Un id público válido: 12 chars del alfabeto nanoid (A-Za-z0-9_-). */
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

/* ------------------------------------------------------------------ */
/* helpers de result set                                               */
/* ------------------------------------------------------------------ */

static const sql_value_t* row_field(const sql_result_t* out, size_t row,
                                    const char* name) {
    if (!out || row >= out->nrows) return NULL;
    for (size_t c = 0; c < out->ncols; c++) {
        const char* cn = out->columns[c].name;
        if (!cn || strcmp(cn, name) != 0) continue;
        return &out->rows[row].cols[c];
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

static const char* field_str(const sql_result_t* out, const char* name) {
    return row_str(out, 0, name);
}

static int64_t row_i64(const sql_result_t* out, size_t row, const char* name,
                       int64_t def) {
    const sql_value_t* v = row_field(out, row, name);
    if (!v || v->is_null || v->type != SQL_COL_INT) return def;
    return v->val.as_int;
}

static int row_bool(const sql_result_t* out, size_t row, const char* name) {
    const sql_value_t* v = row_field(out, row, name);
    if (!v || v->is_null) return 0;
    return v->type == SQL_COL_INT && v->val.as_int != 0;
}

/* "YYYY-MM-DDTHH:MM:SS" (cadena vacía si no es DATETIME). */
static void row_datetime(const sql_result_t* out, size_t row, const char* name,
                         char* buf, size_t cap) {
    buf[0] = '\0';
    const sql_value_t* v = row_field(out, row, name);
    if (!v || v->is_null || v->type != SQL_COL_DATETIME) return;
    const sql_datetime_t* d = &v->val.as_datetime;
    snprintf(buf, cap, "%04d-%02d-%02dT%02d:%02d:%02d", d->year, d->month,
             d->day, d->hour, d->minute, d->second);
}

/* ------------------------------------------------------------------ */
/* string builder                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} strbuf_t;

static int sb_reserve(strbuf_t* b, size_t extra) {
    if (b->len + extra + 1 <= b->cap) return 1;
    size_t ncap = b->cap ? b->cap : 256;
    while (ncap < b->len + extra + 1) ncap *= 2;
    char* np = (char*)realloc(b->data, ncap);
    if (!np) return 0;
    b->data = np;
    b->cap = ncap;
    return 1;
}

static int sb_puts(strbuf_t* b, const char* s) {
    size_t n = strlen(s);
    if (!sb_reserve(b, n)) return 0;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 1;
}

static int sb_printf(strbuf_t* b, const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); return 0; }
    if (!sb_reserve(b, (size_t)n)) { va_end(ap2); return 0; }
    vsnprintf(b->data + b->len, b->cap - b->len, fmt, ap2);
    va_end(ap2);
    b->len += (size_t)n;
    return 1;
}

static void sb_free(strbuf_t* b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

/* Escapa un string a un buffer malloc (con comillas incluidas). */
static char* json_escape_dup(const char* s) {
    if (!s) s = "";
    size_t cap = strlen(s) * 6 + 3;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    char* w = out;
    *w++ = '"';
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        switch (*p) {
            case '"':  *w++ = '\\'; *w++ = '"';  break;
            case '\\': *w++ = '\\'; *w++ = '\\'; break;
            case '\n': *w++ = '\\'; *w++ = 'n';  break;
            case '\r': *w++ = '\\'; *w++ = 'r';  break;
            case '\t': *w++ = '\\'; *w++ = 't';  break;
            default:
                if (*p < 0x20) w += snprintf(w, 7, "\\u%04x", *p);
                else *w++ = (char)*p;
        }
    }
    *w++ = '"';
    *w = '\0';
    return out;
}

/* "%lld" o "null" para reply_to. */
static void reply_buf(char* out, size_t cap, int64_t reply_to) {
    if (reply_to > 0)
        snprintf(out, cap, "%lld", (long long)reply_to);
    else
        snprintf(out, cap, "null");
}

/* Objeto de mensaje para el historial HTTP. */
static int append_history_msg(strbuf_t* b, const sql_result_t* out, size_t row,
                              int64_t self, int first) {
    int64_t msg_id = row_i64(out, row, "msg_id", 0);
    int64_t reply = row_i64(out, row, "msg_id_respuesta", 0);
    int es_mio = row_i64(out, row, "msg_usu_emisor", 0) == self;
    const char* from = row_str(out, row, "emisor_public");
    const char* texto = row_str(out, row, "msg_texto");
    char fecha[32], rbuf[32];
    row_datetime(out, row, "msg_fecha", fecha, sizeof(fecha));
    reply_buf(rbuf, sizeof(rbuf), reply);

    char* jfrom = json_escape_dup(from ? from : "");
    char* jtexto = json_escape_dup(texto ? texto : "");
    int ok = jfrom && jtexto &&
             sb_printf(b,
                       "%s{\"msg_id\":%lld,\"from\":%s,\"es_mio\":%s,"
                       "\"texto\":%s,\"fecha\":\"%s\",\"reply_to\":%s,"
                       "\"leido\":%s}",
                       first ? "" : ",", (long long)msg_id, jfrom,
                       es_mio ? "true" : "false", jtexto, fecha, rbuf,
                       row_bool(out, row, "msg_leido") ? "true" : "false");
    free(jfrom);
    free(jtexto);
    return ok;
}

/* Objeto `dm` para el push WebSocket (sin es_mio: el cliente compara from). */
static int build_ws_dm(strbuf_t* b, int64_t msg_id, const char* from,
                       const char* from_nombre, const char* texto,
                       const char* fecha, int64_t reply) {
    char rbuf[32];
    reply_buf(rbuf, sizeof(rbuf), reply);
    char* jfrom = json_escape_dup(from ? from : "");
    char* jnombre = json_escape_dup(from_nombre ? from_nombre : "");
    char* jtexto = json_escape_dup(texto ? texto : "");
    int ok = jfrom && jnombre && jtexto &&
             sb_printf(b,
                       "{\"msg_id\":%lld,\"from\":%s,\"from_nombre\":%s,"
                       "\"texto\":%s,\"fecha\":\"%s\",\"reply_to\":%s}",
                       (long long)msg_id, jfrom, jnombre, jtexto, fecha, rbuf);
    free(jfrom);
    free(jnombre);
    free(jtexto);
    return ok;
}

/* Resuelve usu_id a partir de un id público.
 * \return >0 usu_id; 0 no existe; -1 error de BD. */
static int64_t resolve_contact(const char* public_id) {
    sql_result_t* dao = users_domain_by_public_id(public_id);
    if (!dao) return -1;
    if (dao->nrows == 0) {
        sql_result_free(dao);
        return 0;
    }
    int64_t id = row_i64(dao, 0, "usu_id", 0);
    sql_result_free(dao);
    return id;
}

/* ------------------------------------------------------------------ */
/* POST /setup                                                         */
/* ------------------------------------------------------------------ */

/*
 * Genera el id público de mensajería del usuario autenticado y lo guarda.
 *   201 {"success":true,"error":0,"msg":"Mensajeria instantanea habilitada",
 *        "data":{"usu_id_public":"<nanoid12>","ws_path":"/ws/v1"}}
 */
static CWS_HANDLER(messaging_setup_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { cws_response_send_error(res, 401); return; }

    char* public_id = jwt_core_nanoid(USU_ID_PUBLIC_LEN);
    if (!public_id) { cws_response_send_error(res, 500); return; }

    sql_result_t* dao = users_domain_set_public_id(u->usu_id, public_id);
    if (!dao) {
        free(public_id);
        cws_response_send_error(res, 500);
        return;
    }
    sql_result_free(dao);

    char body[512];
    int n = snprintf(body, sizeof(body),
                     "{\"success\":true,\"error\":0,"
                     "\"msg\":\"Mensajeria instantanea habilitada\","
                     "\"data\":{\"usu_id_public\":\"%s\",\"ws_path\":\"%s\"}}",
                     public_id, WS_V1_PATH);
    free(public_id);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        cws_response_send_error(res, 500);
        return;
    }
    send_json(res, 201, body);
}

/* ------------------------------------------------------------------ */
/* GET /users/:public_id                                               */
/* ------------------------------------------------------------------ */

static CWS_HANDLER(messaging_user_lookup_handler) {
    char* public_id = param_dup(req, "public_id");
    if (!public_id || !valid_public_id(public_id)) {
        free(public_id);
        send_error_msg(res, 400, "Id publico invalido");
        return;
    }

    sql_result_t* dao = users_domain_by_public_id(public_id);
    if (!dao) {
        free(public_id);
        cws_response_send_error(res, 500);
        return;
    }
    if (dao->nrows == 0) {
        sql_result_free(dao);
        free(public_id);
        send_error_msg(res, 404, "Usuario no encontrado");
        return;
    }

    const char* nombre = field_str(dao, "usu_nombre");
    char nombre_json[608];
    json_escape_to(nombre_json, sizeof(nombre_json), nombre ? nombre : "");
    sql_result_free(dao);

    char body[768];
    int n = snprintf(body, sizeof(body),
                     "{\"success\":true,\"error\":0,"
                     "\"msg\":\"Usuario encontrado\","
                     "\"data\":{\"usu_id_public\":\"%s\","
                     "\"usu_nombre\":\"%s\"}}",
                     public_id, nombre_json);
    free(public_id);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        cws_response_send_error(res, 500);
        return;
    }
    send_json(res, 200, body);
}

/* ------------------------------------------------------------------ */
/* GET /conversations/:public_id/messages                              */
/* ------------------------------------------------------------------ */

static CWS_HANDLER(messaging_history_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { cws_response_send_error(res, 401); return; }

    char* public_id = param_dup(req, "public_id");
    if (!public_id || !valid_public_id(public_id)) {
        free(public_id);
        send_error_msg(res, 400, "Id publico invalido");
        return;
    }
    int64_t otro = resolve_contact(public_id);
    free(public_id);
    if (otro < 0) { cws_response_send_error(res, 500); return; }
    if (otro == 0) { send_error_msg(res, 404, "Usuario no encontrado"); return; }

    int64_t before = 0;
    char* before_s = query_dup(req, "before");
    if (before_s && *before_s) {
        char* end = NULL;
        long long v = strtoll(before_s, &end, 10);
        if (!end || *end || v < 0) {
            free(before_s);
            send_error_msg(res, 400, "Parametro before invalido");
            return;
        }
        before = (int64_t)v;
    }
    free(before_s);

    int limit = CHAT_DEFAULT_LIMIT;
    char* limit_s = query_dup(req, "limit");
    if (limit_s && *limit_s) {
        char* end = NULL;
        long v = strtol(limit_s, &end, 10);
        if (!end || *end || v <= 0) {
            free(limit_s);
            send_error_msg(res, 400, "Parametro limit invalido");
            return;
        }
        limit = v > CHAT_MAX_LIMIT ? CHAT_MAX_LIMIT : (int)v;
    }
    free(limit_s);

    sql_result_t* out = messaging_domain_history(u->usu_id, otro, before, limit);
    if (!out) { cws_response_send_error(res, 500); return; }

    const char* ok = field_str(out, "success");
    if (ok && strcmp(ok, "false") == 0) {
        const char* msg = field_str(out, "msg");
        send_error_msg(res, 400, msg ? msg : "No se pudo obtener el historial");
        sql_result_free(out);
        return;
    }

    strbuf_t b = {0};
    int okb = sb_puts(&b,
                      "{\"success\":true,\"error\":0,"
                      "\"msg\":\"Mensajes obtenidos\","
                      "\"data\":{\"messages\":[");
    for (size_t i = 0; i < out->nrows && okb; i++)
        okb = append_history_msg(&b, out, i, u->usu_id, i == 0);

    int64_t next = 0;
    if (out->nrows == (size_t)limit)
        next = row_i64(out, out->nrows - 1, "msg_id", 0);

    if (okb) {
        if (next > 0)
            okb = sb_printf(&b, "],\"next_before\":%lld}}", (long long)next);
        else
            okb = sb_puts(&b, "],\"next_before\":null}}");
    }
    sql_result_free(out);
    if (!okb) {
        sb_free(&b);
        cws_response_send_error(res, 500);
        return;
    }

    cws_response_body_owned(res, b.data, b.len, CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

/* ------------------------------------------------------------------ */
/* POST /conversations/:public_id/messages                             */
/* ------------------------------------------------------------------ */

static CWS_HANDLER(messaging_send_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { cws_response_send_error(res, 401); return; }

    char* public_id = param_dup(req, "public_id");
    if (!public_id || !valid_public_id(public_id)) {
        free(public_id);
        send_error_msg(res, 400, "Id publico invalido");
        return;
    }
    int64_t otro = resolve_contact(public_id);
    free(public_id);
    if (otro < 0) { cws_response_send_error(res, 500); return; }
    if (otro == 0) { send_error_msg(res, 404, "Usuario no encontrado"); return; }

    char* body = NULL;
    if (req->body && req->body_len > 0 && req->body_len <= 16384) {
        body = (char*)malloc(req->body_len + 1);
        if (body) {
            memcpy(body, req->body, req->body_len);
            body[req->body_len] = '\0';
        }
    }

    char* texto = NULL;
    int64_t reply_to = 0, tmp = 0;
    if (body) {
        jwt_json_get_string(body, "texto", &texto);
        if (jwt_json_get_int(body, "reply_to", &tmp) == 0) reply_to = tmp;
    }
    free(body);

    if (!texto || !*texto) {
        free(texto);
        send_error_msg(res, 400, "El mensaje esta vacio");
        return;
    }
    if (strlen(texto) > WS_MAX_TEXT_BYTES) {
        free(texto);
        send_error_msg(res, 400, "Mensaje demasiado largo");
        return;
    }
    if (reply_to < 0) {
        free(texto);
        send_error_msg(res, 400, "reply_to invalido");
        return;
    }

    sql_result_t* out = messaging_domain_insert(u->usu_id, otro, texto,
                                                reply_to);
    if (!out) {
        free(texto);
        cws_response_send_error(res, 500);
        return;
    }

    const char* ok = field_str(out, "success");
    if (!ok || strcmp(ok, "true") != 0) {
        const char* msg = field_str(out, "msg");
        send_error_msg(res, 400, msg ? msg : "No se pudo enviar el mensaje");
        sql_result_free(out);
        free(texto);
        return;
    }

    int64_t msg_id = row_i64(out, 0, "msg_id", 0);
    int64_t reply = row_i64(out, 0, "msg_id_respuesta", 0);
    const char* from = row_str(out, 0, "emisor_public");
    char fecha[32];
    row_datetime(out, 0, "fecha", fecha, sizeof(fecha));

    /* Push WS `dm` al receptor y a los demás dispositivos del emisor. */
    {
        strbuf_t w = {0};
        if (build_ws_dm(&w, msg_id, from, u->usu_nombre, texto, fecha, reply)) {
            ws_v1_emit_to_user(otro, "dm", w.data, w.len);
            ws_v1_emit_to_user(u->usu_id, "dm", w.data, w.len);
        }
        sb_free(&w);
    }

    /* Respuesta HTTP con el mensaje creado. */
    char rbuf[32];
    reply_buf(rbuf, sizeof(rbuf), reply);
    char* jfrom = json_escape_dup(from ? from : "");
    char* jtexto = json_escape_dup(texto);
    strbuf_t b = {0};
    int okb = jfrom && jtexto &&
              sb_printf(&b,
                        "{\"success\":true,\"error\":0,"
                        "\"msg\":\"Mensaje enviado\","
                        "\"data\":{\"msg_id\":%lld,\"from\":%s,"
                        "\"es_mio\":true,\"texto\":%s,\"fecha\":\"%s\","
                        "\"reply_to\":%s,\"leido\":false}}",
                        (long long)msg_id, jfrom, jtexto, fecha, rbuf);
    free(jfrom);
    free(jtexto);
    free(texto);
    sql_result_free(out);

    if (!okb) {
        sb_free(&b);
        cws_response_send_error(res, 500);
        return;
    }
    cws_response_status(res, 201);
    cws_response_body_owned(res, b.data, b.len, CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

/* ------------------------------------------------------------------ */
/* POST /conversations/:public_id/read                                 */
/* ------------------------------------------------------------------ */

static CWS_HANDLER(messaging_read_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { cws_response_send_error(res, 401); return; }

    char* public_id = param_dup(req, "public_id");
    if (!public_id || !valid_public_id(public_id)) {
        free(public_id);
        send_error_msg(res, 400, "Id publico invalido");
        return;
    }
    int64_t otro = resolve_contact(public_id);
    free(public_id);
    if (otro < 0) { cws_response_send_error(res, 500); return; }
    if (otro == 0) { send_error_msg(res, 404, "Usuario no encontrado"); return; }

    int64_t hasta = 0;
    char* body = NULL;
    if (req->body && req->body_len > 0 && req->body_len <= 16384) {
        body = (char*)malloc(req->body_len + 1);
        if (body) {
            memcpy(body, req->body, req->body_len);
            body[req->body_len] = '\0';
        }
    }
    if (body) {
        int64_t tmp = 0;
        if (jwt_json_get_int(body, "hasta", &tmp) == 0 && tmp > 0)
            hasta = tmp;
    }
    free(body);

    sql_result_t* out = messaging_domain_mark_read(u->usu_id, otro, hasta);
    if (!out) { cws_response_send_error(res, 500); return; }

    const char* ok = field_str(out, "success");
    if (!ok || strcmp(ok, "true") != 0) {
        const char* msg = field_str(out, "msg");
        send_error_msg(res, 400, msg ? msg : "No se pudieron marcar los mensajes");
        sql_result_free(out);
        return;
    }

    int64_t actualizados = row_i64(out, 0, "actualizados", 0);
    sql_result_free(out);

    char resp[192];
    int n = snprintf(resp, sizeof(resp),
                     "{\"success\":true,\"error\":0,"
                     "\"msg\":\"Mensajes marcados como leidos\","
                     "\"data\":{\"actualizados\":%lld}}",
                     (long long)actualizados);
    if (n < 0 || (size_t)n >= sizeof(resp)) {
        cws_response_send_error(res, 500);
        return;
    }
    send_json(res, 200, resp);
}

/* ------------------------------------------------------------------ */
/* /conversations/:public_id/messages (GET historial, POST enviar)     */
/* ------------------------------------------------------------------ */

/* El router de cws guarda un solo método por path; se registra con
 * CWS_M_UNKNOWN y se despacha por método. */
static CWS_HANDLER(messaging_messages_handler) {
    if (req->method == CWS_M_GET)
        messaging_history_handler(req, res);
    else if (req->method == CWS_M_POST)
        messaging_send_handler(req, res);
    else
        send_error_msg(res, 405, "Metodo no permitido");
}

/* ------------------------------------------------------------------ */
/* router                                                              */
/* ------------------------------------------------------------------ */

cws_router_t* messaging_routes(void) {
    cws_router_t* r = cws_router_new();
    if (!r) return NULL;
    cws_router_use(r, cws_mw_authorization_mobile_verify);
    cws_router_add(r, CWS_M_POST, "/setup", messaging_setup_handler);
    cws_router_add(r, CWS_M_GET, "/users/:public_id",
                   messaging_user_lookup_handler);
    cws_router_add(r, CWS_M_UNKNOWN, "/conversations/:public_id/messages",
                   messaging_messages_handler);
    cws_router_add(r, CWS_M_POST, "/conversations/:public_id/read",
                   messaging_read_handler);
    return r;
}
