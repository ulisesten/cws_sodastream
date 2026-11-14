/*
 * messaging_routes.c — mensajería instantánea.
 *
 * POST /api/v1/messaging/setup (Bearer) habilita el chat del usuario:
 *   - genera un identificador público estilo nanoid (12 caracteres,
 *     alfabeto A-Za-z0-9_-), que es el que se usará en el chat 1:1;
 *   - lo persiste en cat_usuarios.usu_id_public con procUsersProc
 *     (tipoRegistro = PROC_USU_ID_PUBLIC_AGREGAR = 3);
 *   - responde 201 con el id y la ruta del WebSocket.
 *
 * `usu_id` es interno y nunca se devuelve.
 *
 * Nota: el endpoint (re)genera el id en cada llamada. El cliente debe
 * guardarlo; si se vuelve a llamar, el identificador cambia.
 */

#define _POSIX_C_SOURCE 200809L

#include "messaging_routes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "authorization_mobile.h"
#include "jwt.h"
#include "../users/domain/users_domain.h"

/* cat_usuarios.usu_id_public es char(12). */
#define USU_ID_PUBLIC_LEN 12
#define WS_V1_PATH        "/ws/v1"

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

/* Copia NUL-terminada de un path-param (cws_request_param no termina el
 * valor si no es el último segmento). Liberar con free. */
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

/* Valor string de una columna de la primera fila. */
static const char* field_str(const sql_result_t* out, const char* name) {
    if (!out || out->nrows == 0) return NULL;
    for (size_t c = 0; c < out->ncols; c++) {
        const char* cn = out->columns[c].name;
        if (!cn || strcmp(cn, name) != 0) continue;
        const sql_value_t* v = &out->rows[0].cols[c];
        if (v->is_null || v->type != SQL_COL_STRING || !v->val.as_string)
            return NULL;
        return v->val.as_string;
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

/*
 * GET /users/:public_id — busca un usuario por su id público de mensajería.
 *  200 {"success":true,"error":0,"msg":"Usuario encontrado",
 *       "data":{"usu_id_public":"<id>","usu_nombre":"<...>"}}
 *  400 id con formato inválido
 *  404 no existe
 *  500 error de BD
 * Nunca devuelve usu_id (interno).
 */
static CWS_HANDLER(messaging_user_lookup_handler) {
    char* public_id = param_dup(req, "public_id");
    if (!public_id || !valid_public_id(public_id)) {
        free(public_id);
        send_json(res, 400,
                  "{\"success\":false,\"error\":1,"
                  "\"msg\":\"Id publico invalido\"}");
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
        send_json(res, 404,
                  "{\"success\":false,\"error\":1,"
                  "\"msg\":\"Usuario no encontrado\"}");
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

/*
 * POST /setup — genera el id público de mensajería del usuario autenticado
 * y lo guarda. Responde:
 *   201 {"success":true,"error":0,"msg":"Mensajeria instantanea habilitada",
 *        "data":{"usu_id_public":"<nanoid12>","ws_path":"/ws/v1"}}
 *   401 sin Bearer válido (lo resuelve el middleware)
 *   500 en error de BD/RNG
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

cws_router_t* messaging_routes(void) {
    cws_router_t* r = cws_router_new();
    if (!r) return NULL;
    cws_router_use(r, cws_mw_authorization_mobile_verify);
    cws_router_add(r, CWS_M_POST, "/setup", messaging_setup_handler);
    cws_router_add(r, CWS_M_GET, "/users/:public_id",
                   messaging_user_lookup_handler);
    return r;
}
