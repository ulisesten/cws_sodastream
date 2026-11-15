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

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <gost/gost.h>

#include "authorization_mobile.h"
#include "configuration.h"
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
#define STICKER_MAX_BYTES  1048576
#define STICKER_MAX_IDS    200

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

/* Copia NUL-terminada de un header. Liberar con free. */
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

/* Sticker id: 64 chars hex (A-F se normaliza a minúsculas). */
static int valid_sticker_id(const char* s) {
    if (!s || strlen(s) != 64) return 0;
    for (const char* p = s; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
              (*p >= 'A' && *p <= 'F')))
            return 0;
    }
    return 1;
}

static void lowercase_inplace(char* s) {
    for (; s && *s; s++)
        if (*s >= 'A' && *s <= 'Z') *s = (char)(*s - 'A' + 'a');
}

static void hex_encode32(const uint8_t* d, char out[65]) {
    static const char h[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = h[d[i] >> 4];
        out[i * 2 + 1] = h[d[i] & 0x0f];
    }
    out[64] = '\0';
}

static uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Valida WebP y extrae dimensiones/animación. 0 = válido. */
static int webp_info(const uint8_t* d, size_t n, int* w, int* h, int* anim) {
    if (!d || n < 30) return -1;
    if (memcmp(d, "RIFF", 4) != 0 || memcmp(d + 8, "WEBP", 4) != 0)
        return -1;
    if ((uint64_t)le32(d + 4) + 8 > n) return -1;

    const uint8_t* c = d + 12;
    size_t left = n - 12;
    *anim = 0;

    if (left >= 18 && memcmp(c, "VP8X", 4) == 0) {
        uint32_t sz = le32(c + 4);
        if (sz < 10 || left < 8 + (size_t)sz) return -1;
        const uint8_t* p = c + 8;
        if (p[0] & 0x02) *anim = 1;
        *w = 1 + (p[4] | (p[5] << 8) | (p[6] << 16));
        *h = 1 + (p[7] | (p[8] << 8) | (p[9] << 16));
        return 0;
    }
    if (left >= 18 && memcmp(c, "VP8 ", 4) == 0) {
        uint32_t sz = le32(c + 4);
        if (sz < 10 || left < 8 + (size_t)sz) return -1;
        const uint8_t* p = c + 8;
        if (p[3] != 0x9d || p[4] != 0x01 || p[5] != 0x2a) return -1;
        *w = (p[6] | (p[7] << 8)) & 0x3fff;
        *h = (p[8] | (p[9] << 8)) & 0x3fff;
        return 0;
    }
    if (left >= 13 && memcmp(c, "VP8L", 4) == 0) {
        uint32_t sz = le32(c + 4);
        if (sz < 5 || left < 8 + (size_t)sz) return -1;
        const uint8_t* p = c + 8;
        if (p[0] != 0x2f) return -1;
        uint32_t bits = (uint32_t)p[1] | ((uint32_t)p[2] << 8) |
                        ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24);
        *w = (int)(bits & 0x3fff) + 1;
        *h = (int)((bits >> 14) & 0x3fff) + 1;
        return 0;
    }
    return -1;
}

/* Ruta STICKERS_DIR/<id>.webp (id ya validado). */
static int sticker_path(const char* id, char* out, size_t cap) {
    const char* dir = messaging_domain_stickers_dir();
    int n = snprintf(out, cap, "%s/%s.webp", dir, id);
    return n > 0 && (size_t)n < cap;
}

static void free_string_array(char** arr, size_t n) {
    for (size_t i = 0; i < n; i++) free(arr[i]);
    free(arr);
}

/* Extrae un array de strings JSON: {"name":["a","b"]}. 0 = ok. */
static int json_get_string_array(const char* json, const char* name,
                                 char*** out, size_t* nout, size_t max) {
    *out = NULL;
    *nout = 0;
    if (!json || !name) return 0;

    char key[128];
    int kn = snprintf(key, sizeof(key), "\"%s\"", name);
    if (kn < 0 || (size_t)kn >= sizeof(key)) return -1;

    const char* p = strstr(json, key);
    if (!p) return 0;
    p += (size_t)kn;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != ':') return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '[') return -1;
    p++;

    char** arr = NULL;
    size_t n = 0;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
               *p == ',')
            p++;
        if (*p == ']') { p++; break; }
        if (*p != '"') { free_string_array(arr, n); return -1; }
        p++;
        const char* start = p;
        while (*p && *p != '"') {
            if (*p == '\\') { free_string_array(arr, n); return -1; }
            p++;
        }
        if (*p != '"') { free_string_array(arr, n); return -1; }
        size_t len = (size_t)(p - start);
        if (n >= max) { free_string_array(arr, n); return -1; }
        char** na = (char**)realloc(arr, (n + 1) * sizeof(char*));
        if (!na) { free_string_array(arr, n); return -1; }
        arr = na;
        arr[n] = (char*)malloc(len + 1);
        if (!arr[n]) { free_string_array(arr, n); return -1; }
        memcpy(arr[n], start, len);
        arr[n][len] = '\0';
        n++;
        p++;
    }
    *out = arr;
    *nout = n;
    return 0;
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
    const char* sid = row_str(out, row, "msg_sticker_id");
    const char* smime = row_str(out, row, "sticker_mime");
    int is_sticker = row_i64(out, row, "msg_tipo", 0) == 1 && sid && *sid;
    int sw = (int)row_i64(out, row, "sticker_width", 0);
    int sh = (int)row_i64(out, row, "sticker_height", 0);
    int sanim = row_bool(out, row, "sticker_animated");

    char fecha[32], rbuf[32], sid_field[96], sticker_json[640];
    row_datetime(out, row, "msg_fecha", fecha, sizeof(fecha));
    reply_buf(rbuf, sizeof(rbuf), reply);

    sticker_json[0] = '\0';
    if (is_sticker) {
        snprintf(sid_field, sizeof(sid_field), "\"%s\"", sid);
        snprintf(sticker_json, sizeof(sticker_json),
                 ",\"sticker\":{\"sticker_id\":\"%s\",\"mime\":\"%s\","
                 "\"width\":%d,\"height\":%d,\"animated\":%s,"
                 "\"url\":\"/api/v1/messaging/stickers/%s/file\"}",
                 sid, smime ? smime : "image/webp", sw, sh,
                 sanim ? "true" : "false", sid);
    } else {
        snprintf(sid_field, sizeof(sid_field), "null");
    }

    char* jfrom = json_escape_dup(from ? from : "");
    char* jtexto = json_escape_dup(texto ? texto : "");
    int ok = jfrom && jtexto &&
             sb_printf(b,
                       "%s{\"msg_id\":%lld,\"from\":%s,\"es_mio\":%s,"
                       "\"tipo\":\"%s\",\"sticker_id\":%s,\"texto\":%s,"
                       "\"fecha\":\"%s\",\"reply_to\":%s,\"leido\":%s",
                       first ? "" : ",", (long long)msg_id, jfrom,
                       es_mio ? "true" : "false",
                       is_sticker ? "sticker" : "text", sid_field, jtexto,
                       fecha, rbuf,
                       row_bool(out, row, "msg_leido") ? "true" : "false");
    if (ok && sticker_json[0]) ok = sb_puts(b, sticker_json);
    if (ok) ok = sb_puts(b, "}");
    free(jfrom);
    free(jtexto);
    return ok;
}

/* Objeto `dm` para el push WebSocket (sin es_mio: el cliente compara from). */
static int build_ws_dm(strbuf_t* b, int64_t msg_id, const char* from,
                       const char* from_nombre, const char* tipo,
                       const char* sticker_id, const char* mime,
                       int sw, int sh, int sanim, const char* texto,
                       const char* fecha, int64_t reply) {
    char rbuf[32], sid_field[96], sticker_json[640];
    reply_buf(rbuf, sizeof(rbuf), reply);

    int is_sticker = tipo && strcmp(tipo, "sticker") == 0 &&
                     sticker_id && *sticker_id;
    sticker_json[0] = '\0';
    if (is_sticker) {
        snprintf(sid_field, sizeof(sid_field), "\"%s\"", sticker_id);
        snprintf(sticker_json, sizeof(sticker_json),
                 ",\"sticker\":{\"sticker_id\":\"%s\",\"mime\":\"%s\","
                 "\"width\":%d,\"height\":%d,\"animated\":%s,"
                 "\"url\":\"/api/v1/messaging/stickers/%s/file\"}",
                 sticker_id, mime ? mime : "image/webp", sw, sh,
                 sanim ? "true" : "false", sticker_id);
    } else {
        snprintf(sid_field, sizeof(sid_field), "null");
    }

    char* jfrom = json_escape_dup(from ? from : "");
    char* jnombre = json_escape_dup(from_nombre ? from_nombre : "");
    char* jtexto = json_escape_dup(texto ? texto : "");
    int ok = jfrom && jnombre && jtexto &&
             sb_printf(b,
                       "{\"msg_id\":%lld,\"from\":%s,\"from_nombre\":%s,"
                       "\"tipo\":\"%s\",\"sticker_id\":%s,\"texto\":%s,"
                       "\"fecha\":\"%s\",\"reply_to\":%s",
                       (long long)msg_id, jfrom, jnombre,
                       is_sticker ? "sticker" : "text", sid_field, jtexto,
                       fecha, rbuf);
    if (ok && sticker_json[0]) ok = sb_puts(b, sticker_json);
    if (ok) ok = sb_puts(b, "}");
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
/* helpers de stickers                                                 */
/* ------------------------------------------------------------------ */

/* CSV de ids válidos, normalizados y sin duplicados.
 * *bad: 0 ok, 1 id inválido, 2 sin memoria. */
static char* build_ids_csv(char** ids, size_t n, int* bad) {
    *bad = 0;
    if (n == 0) {
        char* empty = (char*)malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    char** norm = (char**)calloc(n, sizeof(char*));
    if (!norm) { *bad = 2; return NULL; }
    size_t m = 0;
    for (size_t i = 0; i < n && *bad == 0; i++) {
        if (!valid_sticker_id(ids[i])) { *bad = 1; break; }
        char* c = strdup(ids[i]);
        if (!c) { *bad = 2; break; }
        lowercase_inplace(c);
        int dup = 0;
        for (size_t k = 0; k < m; k++)
            if (strcmp(norm[k], c) == 0) { dup = 1; break; }
        if (dup) { free(c); continue; }
        norm[m++] = c;
    }
    char* csv = NULL;
    if (*bad == 0) {
        csv = (char*)malloc(m * 65 + 1);
        if (!csv) *bad = 2;
        else {
            size_t off = 0;
            for (size_t i = 0; i < m; i++) {
                if (i) csv[off++] = ',';
                memcpy(csv + off, norm[i], 64);
                off += 64;
            }
            csv[off] = '\0';
        }
    }
    for (size_t i = 0; i < m; i++) free(norm[i]);
    free(norm);
    return csv;
}

static int append_json_id(strbuf_t* b, const char* id, int* first) {
    if (!*first) sb_puts(b, ",");
    *first = 0;
    return sb_printf(b, "\"%s\"", id);
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
/* GET /users                                                          */
/* ------------------------------------------------------------------ */

/*
 * Lista de usuarios con mensajería habilitada (excluye al propio).
 *  200 {"success":true,"error":0,"msg":"Usuarios obtenidos",
 *       "data":[{"usu_id_public":"...","usu_nombre":"..."}]}
 * El cliente puede filtrar la lista localmente. Nunca se devuelve usu_id.
 */
static CWS_HANDLER(messaging_users_list_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { cws_response_send_error(res, 401); return; }

    sql_result_t* out = users_domain_list(u->usu_id);
    if (!out) { cws_response_send_error(res, 500); return; }

    const char* ok = field_str(out, "success");
    if (ok && strcmp(ok, "false") == 0) {
        const char* msg = field_str(out, "msg");
        send_error_msg(res, 400, msg ? msg : "No se pudo obtener la lista");
        sql_result_free(out);
        return;
    }

    strbuf_t b = {0};
    int okb = sb_puts(&b,
                      "{\"success\":true,\"error\":0,"
                      "\"msg\":\"Usuarios obtenidos\",\"data\":[");
    for (size_t i = 0; i < out->nrows && okb; i++) {
        const char* pid = row_str(out, i, "usu_id_public");
        const char* nom = row_str(out, i, "usu_nombre");
        char* jpid = json_escape_dup(pid ? pid : "");
        char* jnom = json_escape_dup(nom ? nom : "");
        okb = jpid && jnom &&
              sb_printf(&b, "%s{\"usu_id_public\":%s,\"usu_nombre\":%s}",
                        i == 0 ? "" : ",", jpid, jnom);
        free(jpid);
        free(jnom);
    }
    if (okb) okb = sb_puts(&b, "]}");
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
    char* tipo = NULL;
    char* sticker_raw = NULL;
    int64_t reply_to = 0, tmp = 0;
    if (body) {
        jwt_json_get_string(body, "texto", &texto);
        jwt_json_get_string(body, "tipo", &tipo);
        jwt_json_get_string(body, "sticker_id", &sticker_raw);
        if (jwt_json_get_int(body, "reply_to", &tmp) == 0) reply_to = tmp;
    }
    free(body);

    if (!tipo || !*tipo) {
        free(tipo);
        tipo = strdup("text");
    }
    if (!tipo) {
        free(texto);
        free(sticker_raw);
        cws_response_send_error(res, 500);
        return;
    }
    lowercase_inplace(tipo);

    int is_sticker = strcmp(tipo, "sticker") == 0;
    if (!is_sticker && strcmp(tipo, "text") != 0) {
        free(texto);
        free(tipo);
        free(sticker_raw);
        send_error_msg(res, 400, "Tipo de mensaje invalido");
        return;
    }
    if (reply_to < 0) {
        free(texto);
        free(tipo);
        free(sticker_raw);
        send_error_msg(res, 400, "reply_to invalido");
        return;
    }

    char sid[65] = {0};
    if (is_sticker) {
        if (!sticker_raw || !valid_sticker_id(sticker_raw)) {
            free(texto);
            free(tipo);
            free(sticker_raw);
            send_error_msg(res, 400, "Sticker id invalido");
            return;
        }
        snprintf(sid, sizeof(sid), "%s", sticker_raw);
        lowercase_inplace(sid);
        if (texto && *texto) {
            free(texto);
            free(tipo);
            free(sticker_raw);
            send_error_msg(res, 400, "texto no permitido en sticker");
            return;
        }
        sql_result_t* st = messaging_domain_sticker_get(sid);
        if (!st) {
            free(texto);
            free(tipo);
            free(sticker_raw);
            cws_response_send_error(res, 500);
            return;
        }
        if (st->nrows == 0) {
            sql_result_free(st);
            free(texto);
            free(tipo);
            free(sticker_raw);
            send_error_msg(res, 404, "Sticker no registrado");
            return;
        }
        sql_result_free(st);
        free(texto);
        texto = strdup("");
        if (!texto) {
            free(tipo);
            free(sticker_raw);
            cws_response_send_error(res, 500);
            return;
        }
    } else {
        if (!texto || !*texto) {
            free(texto);
            free(tipo);
            free(sticker_raw);
            send_error_msg(res, 400, "El mensaje esta vacio");
            return;
        }
        if (strlen(texto) > WS_MAX_TEXT_BYTES) {
            free(texto);
            free(tipo);
            free(sticker_raw);
            send_error_msg(res, 400, "Mensaje demasiado largo");
            return;
        }
    }
    free(sticker_raw);

    sql_result_t* out = messaging_domain_insert(u->usu_id, otro,
                                                is_sticker ? 1 : 0,
                                                is_sticker ? sid : NULL,
                                                texto, reply_to);
    if (!out) {
        free(texto);
        free(tipo);
        cws_response_send_error(res, 500);
        return;
    }

    const char* ok = field_str(out, "success");
    if (!ok || strcmp(ok, "true") != 0) {
        const char* msg = field_str(out, "msg");
        send_error_msg(res, 400, msg ? msg : "No se pudo enviar el mensaje");
        sql_result_free(out);
        free(texto);
        free(tipo);
        return;
    }

    int64_t msg_id = row_i64(out, 0, "msg_id", 0);
    int64_t reply = row_i64(out, 0, "msg_id_respuesta", 0);
    const char* from = row_str(out, 0, "emisor_public");
    const char* smime = row_str(out, 0, "sticker_mime");
    int sw = (int)row_i64(out, 0, "sticker_width", 0);
    int sh = (int)row_i64(out, 0, "sticker_height", 0);
    int sanim = row_bool(out, 0, "sticker_animated");
    char fecha[32];
    row_datetime(out, 0, "fecha", fecha, sizeof(fecha));

    /* Push WS `dm` al receptor y a los demás dispositivos del emisor. */
    {
        strbuf_t w = {0};
        if (build_ws_dm(&w, msg_id, from, u->usu_nombre, tipo,
                        is_sticker ? sid : NULL, smime, sw, sh, sanim,
                        texto, fecha, reply)) {
            ws_v1_emit_to_user(otro, "dm", w.data, w.len);
            ws_v1_emit_to_user(u->usu_id, "dm", w.data, w.len);
        }
        sb_free(&w);
    }

    /* Respuesta HTTP con el mensaje creado. */
    char rbuf[32], sid_field[96], sticker_json[640];
    reply_buf(rbuf, sizeof(rbuf), reply);
    sticker_json[0] = '\0';
    if (is_sticker) {
        snprintf(sid_field, sizeof(sid_field), "\"%s\"", sid);
        snprintf(sticker_json, sizeof(sticker_json),
                 ",\"sticker\":{\"sticker_id\":\"%s\",\"mime\":\"%s\","
                 "\"width\":%d,\"height\":%d,\"animated\":%s,"
                 "\"url\":\"/api/v1/messaging/stickers/%s/file\"}",
                 sid, smime ? smime : "image/webp", sw, sh,
                 sanim ? "true" : "false", sid);
    } else {
        snprintf(sid_field, sizeof(sid_field), "null");
    }

    char* jfrom = json_escape_dup(from ? from : "");
    char* jtexto = json_escape_dup(texto);
    strbuf_t b = {0};
    int okb = jfrom && jtexto &&
              sb_printf(&b,
                        "{\"success\":true,\"error\":0,"
                        "\"msg\":\"Mensaje enviado\","
                        "\"data\":{\"msg_id\":%lld,\"from\":%s,"
                        "\"es_mio\":true,\"tipo\":\"%s\","
                        "\"sticker_id\":%s,\"texto\":%s,\"fecha\":\"%s\","
                        "\"reply_to\":%s,\"leido\":false",
                        (long long)msg_id, jfrom,
                        is_sticker ? "sticker" : "text", sid_field, jtexto,
                        fecha, rbuf);
    if (okb && sticker_json[0]) okb = sb_puts(&b, sticker_json);
    if (okb) okb = sb_puts(&b, "}}");
    free(jfrom);
    free(jtexto);
    free(texto);
    free(tipo);
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
/* POST /stickers                                                     */
/* ------------------------------------------------------------------ */

static CWS_HANDLER(messaging_sticker_upload_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { cws_response_send_error(res, 401); return; }

    char* ct = header_dup(req, "content-type");
    int is_webp = 0;
    if (ct) {
        const char* p = ct;
        while (*p == ' ' || *p == '\t') p++;
        if (strncasecmp(p, "image/webp", 10) == 0) {
            char c = p[10];
            if (c == '\0' || c == ';' || c == ' ' || c == '\t') is_webp = 1;
        }
    }
    free(ct);
    if (!is_webp) {
        send_error_msg(res, 415, "Mime no soportado (image/webp)");
        return;
    }

    if (!req->body || req->body_len == 0) {
        send_error_msg(res, 400, "Sticker vacio");
        return;
    }
    if (req->body_len > STICKER_MAX_BYTES) {
        send_error_msg(res, 413, "Sticker demasiado grande");
        return;
    }

    const uint8_t* data = (const uint8_t*)req->body;
    int w = 0, h = 0, anim = 0;
    if (webp_info(data, req->body_len, &w, &h, &anim) != 0) {
        send_error_msg(res, 400, "WebP invalido");
        return;
    }
    if (w != 512 || h != 512) {
        send_error_msg(res, 400, "Dimensiones invalidas (512x512)");
        return;
    }

    uint8_t digest[32];
    if (gost_hash(data, req->body_len, GOST_HASH_256, digest) != GOST_OK) {
        cws_response_send_error(res, 500);
        return;
    }
    char id[65];
    hex_encode32(digest, id);

    char* xid = header_dup(req, "x-sticker-id");
    if (xid) {
        lowercase_inplace(xid);
        int same = strcmp(xid, id) == 0;
        free(xid);
        if (!same) {
            send_error_msg(res, 400, "X-Sticker-Id no coincide");
            return;
        }
    }

    char path[1200];
    if (!sticker_path(id, path, sizeof(path))) {
        cws_response_send_error(res, 500);
        return;
    }
    int existed = (access(path, F_OK) == 0);
    if (!existed) {
        char tmp[1300];
        snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
        FILE* f = fopen(tmp, "wb");
        if (!f) { cws_response_send_error(res, 500); return; }
        size_t wr = fwrite(data, 1, req->body_len, f);
        int cl = fclose(f);
        if (cl != 0 || wr != req->body_len) {
            unlink(tmp);
            cws_response_send_error(res, 500);
            return;
        }
        if (rename(tmp, path) != 0) {
            unlink(tmp);
            cws_response_send_error(res, 500);
            return;
        }
    }

    sql_result_t* out = messaging_domain_sticker_insert(
        id, "image/webp", (int64_t)req->body_len, w, h, anim, u->usu_id);
    if (!out) {
        if (!existed) unlink(path);
        cws_response_send_error(res, 500);
        return;
    }
    const char* ok = field_str(out, "success");
    if (!ok || strcmp(ok, "true") != 0) {
        const char* msg = field_str(out, "msg");
        send_error_msg(res, 400,
                       msg ? msg : "No se pudo registrar el sticker");
        sql_result_free(out);
        if (!existed) unlink(path);
        return;
    }
    int64_t exists = row_i64(out, 0, "exists", 0);
    sql_result_free(out);

    char body[768];
    int n = snprintf(
        body, sizeof(body),
        "{\"success\":true,\"error\":0,\"msg\":\"Sticker registrado\","
        "\"data\":{\"sticker_id\":\"%s\",\"mime\":\"image/webp\","
        "\"bytes\":%lld,\"width\":%d,\"height\":%d,\"animated\":%s,"
        "\"exists\":%s,\"url\":\"/api/v1/messaging/stickers/%s/file\"}}",
        id, (long long)req->body_len, w, h, anim ? "true" : "false",
        exists ? "true" : "false", id);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        cws_response_send_error(res, 500);
        return;
    }
    send_json(res, exists ? 200 : 201, body);
}

/* ------------------------------------------------------------------ */
/* GET /stickers/:sticker_id                                          */
/* ------------------------------------------------------------------ */

static CWS_HANDLER(messaging_sticker_get_handler) {
    char* raw = param_dup(req, "sticker_id");
    if (!raw || !valid_sticker_id(raw)) {
        free(raw);
        send_error_msg(res, 400, "Sticker id invalido");
        return;
    }
    lowercase_inplace(raw);

    sql_result_t* out = messaging_domain_sticker_get(raw);
    if (!out) {
        free(raw);
        cws_response_send_error(res, 500);
        return;
    }
    if (out->nrows == 0) {
        sql_result_free(out);
        free(raw);
        send_error_msg(res, 404, "Sticker no encontrado");
        return;
    }

    const char* mime = row_str(out, 0, "sti_mime");
    int64_t bytes = row_i64(out, 0, "sti_bytes", 0);
    int w = (int)row_i64(out, 0, "sti_width", 0);
    int h = (int)row_i64(out, 0, "sti_height", 0);
    int anim = row_bool(out, 0, "sti_animated");
    char mime_json[64];
    json_escape_to(mime_json, sizeof(mime_json), mime ? mime : "image/webp");
    sql_result_free(out);

    char body[640];
    int n = snprintf(
        body, sizeof(body),
        "{\"success\":true,\"error\":0,\"msg\":\"Sticker encontrado\","
        "\"data\":{\"sticker_id\":\"%s\",\"mime\":%s,\"bytes\":%lld,"
        "\"width\":%d,\"height\":%d,\"animated\":%s,"
        "\"url\":\"/api/v1/messaging/stickers/%s/file\"}}",
        raw, mime_json, (long long)bytes, w, h,
        anim ? "true" : "false", raw);
    free(raw);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        cws_response_send_error(res, 500);
        return;
    }
    send_json(res, 200, body);
}

/* ------------------------------------------------------------------ */
/* GET /stickers/:sticker_id/file                                     */
/* ------------------------------------------------------------------ */

static CWS_HANDLER(messaging_sticker_file_handler) {
    char* raw = param_dup(req, "sticker_id");
    if (!raw || !valid_sticker_id(raw)) {
        free(raw);
        send_error_msg(res, 400, "Sticker id invalido");
        return;
    }
    lowercase_inplace(raw);

    char path[1200];
    if (!sticker_path(raw, path, sizeof(path))) {
        free(raw);
        cws_response_send_error(res, 500);
        return;
    }

    char etag[70];
    snprintf(etag, sizeof(etag), "\"%s\"", raw);

    char* inm = header_dup(req, "if-none-match");
    if (inm && strcmp(inm, etag) == 0) {
        free(inm);
        free(raw);
        cws_response_status(res, 304);
        cws_response_header(res, "ETag", "%s", etag);
        cws_response_send(res);
        return;
    }
    free(inm);

    cws_response_header(res, "Cache-Control",
                        "private, max-age=31536000, immutable");
    cws_response_header(res, "ETag", "%s", etag);
    cws_response_sendfile_ex(res, path, "image/webp", 0);
    free(raw);
}

/* ------------------------------------------------------------------ */
/* POST /stickers/check                                               */
/* ------------------------------------------------------------------ */

static CWS_HANDLER(messaging_stickers_check_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { cws_response_send_error(res, 401); return; }

    char* body = NULL;
    if (req->body && req->body_len > 0 && req->body_len <= 65536) {
        body = (char*)malloc(req->body_len + 1);
        if (body) {
            memcpy(body, req->body, req->body_len);
            body[req->body_len] = '\0';
        }
    }

    char* contact = NULL;
    char** ids = NULL;
    size_t nids = 0;
    int parse_err = 0;
    if (body) {
        jwt_json_get_string(body, "contact_public_id", &contact);
        if (json_get_string_array(body, "sticker_ids", &ids, &nids,
                                  STICKER_MAX_IDS) != 0)
            parse_err = 1;
    }
    free(body);
    if (parse_err) {
        free_string_array(ids, nids);
        free(contact);
        send_error_msg(res, 400, "sticker_ids invalido");
        return;
    }

    int64_t contact_usu = 0;
    if (contact && *contact) {
        if (!valid_public_id(contact)) {
            free_string_array(ids, nids);
            free(contact);
            send_error_msg(res, 400, "contact_public_id invalido");
            return;
        }
        contact_usu = resolve_contact(contact);
        if (contact_usu < 0) {
            free_string_array(ids, nids);
            free(contact);
            cws_response_send_error(res, 500);
            return;
        }
        if (contact_usu == 0) {
            free_string_array(ids, nids);
            free(contact);
            send_error_msg(res, 404, "Usuario no encontrado");
            return;
        }
    }
    free(contact);

    int bad = 0;
    char* csv = build_ids_csv(ids, nids, &bad);
    free_string_array(ids, nids);
    if (bad) {
        free(csv);
        if (bad == 2) cws_response_send_error(res, 500);
        else send_error_msg(res, 400, "sticker_ids invalido");
        return;
    }

    sql_result_t* out = messaging_domain_stickers_check(csv, contact_usu);
    free(csv);
    if (!out) { cws_response_send_error(res, 500); return; }

    strbuf_t b = {0};
    int okb = sb_puts(&b,
                      "{\"success\":true,\"error\":0,"
                      "\"msg\":\"Stickers verificados\",\"data\":{");
    int first = 1;
    if (okb) okb = sb_puts(&b, "\"registered\":[");
    for (size_t i = 0; i < out->nrows && okb; i++) {
        if (row_bool(out, i, "registered"))
            okb = append_json_id(&b, row_str(out, i, "sti_hash"), &first);
    }
    first = 1;
    if (okb) okb = sb_puts(&b, "],\"missing\":[");
    for (size_t i = 0; i < out->nrows && okb; i++) {
        if (!row_bool(out, i, "registered"))
            okb = append_json_id(&b, row_str(out, i, "sti_hash"), &first);
    }
    if (okb && contact_usu > 0) {
        first = 1;
        okb = sb_puts(&b, "],\"contact_has\":[");
        for (size_t i = 0; i < out->nrows && okb; i++) {
            if (row_bool(out, i, "contact_has"))
                okb = append_json_id(&b, row_str(out, i, "sti_hash"), &first);
        }
        first = 1;
        if (okb) okb = sb_puts(&b, "],\"contact_missing\":[");
        for (size_t i = 0; i < out->nrows && okb; i++) {
            if (!row_bool(out, i, "contact_has"))
                okb = append_json_id(&b, row_str(out, i, "sti_hash"), &first);
        }
    }
    if (okb) okb = sb_puts(&b, "]}}");
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
/* POST /stickers/sync                                                */
/* ------------------------------------------------------------------ */

static CWS_HANDLER(messaging_stickers_sync_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { cws_response_send_error(res, 401); return; }

    char* body = NULL;
    if (req->body && req->body_len > 0 && req->body_len <= 65536) {
        body = (char*)malloc(req->body_len + 1);
        if (body) {
            memcpy(body, req->body, req->body_len);
            body[req->body_len] = '\0';
        }
    }

    char** ids = NULL;
    size_t nids = 0;
    int parse_err = 0;
    if (body) {
        if (json_get_string_array(body, "sticker_ids", &ids, &nids,
                                  STICKER_MAX_IDS) != 0)
            parse_err = 1;
    }
    free(body);
    if (parse_err) {
        free_string_array(ids, nids);
        send_error_msg(res, 400, "sticker_ids invalido");
        return;
    }

    int bad = 0;
    char* csv = build_ids_csv(ids, nids, &bad);
    free_string_array(ids, nids);
    if (bad) {
        free(csv);
        if (bad == 2) cws_response_send_error(res, 500);
        else send_error_msg(res, 400, "sticker_ids invalido");
        return;
    }

    sql_result_t* out = messaging_domain_stickers_sync(u->usu_id, csv);
    free(csv);
    if (!out) { cws_response_send_error(res, 500); return; }

    const char* ok = field_str(out, "success");
    if (!ok || strcmp(ok, "true") != 0) {
        const char* msg = field_str(out, "msg");
        send_error_msg(res, 400, msg ? msg : "No se sincronizo el inventario");
        sql_result_free(out);
        return;
    }
    int64_t synced = row_i64(out, 0, "synced", 0);
    sql_result_free(out);

    char resp[192];
    int n = snprintf(resp, sizeof(resp),
                     "{\"success\":true,\"error\":0,"
                     "\"msg\":\"Inventario sincronizado\","
                     "\"data\":{\"synced\":%lld}}",
                     (long long)synced);
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
    cws_router_add(r, CWS_M_GET, "/users",
                   messaging_users_list_handler);
    cws_router_add(r, CWS_M_GET, "/users/:public_id",
                   messaging_user_lookup_handler);
    cws_router_add(r, CWS_M_POST, "/stickers",
                   messaging_sticker_upload_handler);
    cws_router_add(r, CWS_M_POST, "/stickers/check",
                   messaging_stickers_check_handler);
    cws_router_add(r, CWS_M_POST, "/stickers/sync",
                   messaging_stickers_sync_handler);
    cws_router_add(r, CWS_M_GET, "/stickers/:sticker_id",
                   messaging_sticker_get_handler);
    cws_router_add(r, CWS_M_GET, "/stickers/:sticker_id/file",
                   messaging_sticker_file_handler);
    cws_router_add(r, CWS_M_UNKNOWN, "/conversations/:public_id/messages",
                   messaging_messages_handler);
    cws_router_add(r, CWS_M_POST, "/conversations/:public_id/read",
                   messaging_read_handler);
    return r;
}
