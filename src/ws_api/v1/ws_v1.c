/*
 * ws_v1.c — WebSocket API v1.
 *
 * Inicializa el hub WebSocket de la lib cws y monta el endpoint en /ws/v1.
 * El handshake usa GOST (Streebog-256) en lugar de SHA-1 (ver WS_README.md).
 *
 * AUTENTICACIÓN OBLIGATORIA: el upgrade exige
 *   Authorization: Bearer <access_token>   (canal móvil/Bearer)
 * validado con firma + expiración + session_type + salt contra el DAO. Sin
 * token válido se rechaza con 401 y no se completa el upgrade.
 *
 * Identidad:
 *   - usu_id: interno. NUNCA se emite en welcome/identity/mensajes.
 *   - nombre: usu_nombre del token (autoritativo).
 *   - public_id: id público de la conexión (socket_id, 24 hex). Es el que
 *     viaja en los mensajes junto al nombre.
 *
 * Protocolo de aplicación (envelope): `evento '\n' payload`.
 * Cliente -> servidor: ping, echo, join, leave, message, whoami, stats.
 * Servidor -> cliente: welcome, pong, echo, joined, left, message, identity,
 * stats, error.
 */

#include "ws_v1.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "authorization_mobile.h"
#include "jwt.h"
#include "integrations/push/push_outbox_domain.h"

#define WS_V1_PATH        "/ws/v1"
#define WS_V1_LOBBY       "lobby"
#define WS_V1_MAX_TEXT    4096

static cws_ws_t* g_ws = NULL;

/* Contexto por conexión (cws_ws_socket_data). */
typedef struct ws_identity {
    int64_t usu_id;  /* interno, nunca se emite */
    char*   nombre;  /* usu_nombre autenticado */
} ws_identity_t;

/* --- helpers ------------------------------------------------------------- */

/* Header NUL-terminado malloc (los headers de cws no lo están). */
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

/* Authorization: Bearer <token> */
static char* bearer_token(const cws_request_t* req) {
    char* h = header_dup(req, "authorization");
    if (!h) return NULL;
    const char* p = h;
    while (*p == ' ' || *p == '\t') p++;
    if (strncasecmp(p, "Bearer", 6) != 0) { free(h); return NULL; }
    p += 6;
    while (*p == ' ' || *p == '\t') p++;
    char* tok = strdup(p);
    if (tok) {
        size_t l = strlen(tok);
        while (l > 0 && (tok[l - 1] == '\r' || tok[l - 1] == '\n' ||
                         tok[l - 1] == ' ' || tok[l - 1] == '\t'))
            tok[--l] = '\0';
    }
    free(h);
    return tok;
}

/* Escapa un string NUL-terminado para JSON en un buffer malloc. */
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

/* Rechazo JSON en el upgrade (el 101 aún no se escribió). */
static void ws_reject(cws_response_t* res, int status, const char* msg) {
    char body[256];
    int n = snprintf(body, sizeof(body),
                     "{\"success\":false,\"error\":1,\"msg\":\"%s\"}", msg);
    if (n < 0 || (size_t)n >= sizeof(body)) {
        cws_response_send_error(res, status);
        return;
    }
    cws_response_status(res, status);
    cws_response_body(res, body, (size_t)n, CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

/* {"public_id":"<socket_id>","nombre":"<usu_nombre>"} (sin usu_id). */
static void identity_json(cws_ws_socket_t* s, const ws_identity_t* id,
                          char* out, size_t cap) {
    char* pub = json_escape_dup(cws_ws_socket_id(s));
    char* nom = json_escape_dup(id && id->nombre ? id->nombre : "");
    if (!pub || !nom) {
        snprintf(out, cap, "null");
        free(pub);
        free(nom);
        return;
    }
    snprintf(out, cap, "{\"public_id\":%s,\"nombre\":%s}", pub, nom);
    free(pub);
    free(nom);
}

/* --- callbacks ----------------------------------------------------------- */

/*
 * Upgrade: exige Bearer válido (firma + exp + salt). Crea la identidad del
 * socket. Sin token o inválido -> 401 (no hay invitados).
 */
static int ws_upgrade(cws_ws_socket_t* s, cws_request_t* req,
                      cws_response_t* res, void* ud) {
    (void)ud;

    char* token = bearer_token(req);
    if (!token || !*token) {
        free(token);
        ws_reject(res, 401, "No credentials are present.");
        return CWS_ERR_INVALID;
    }

    jwt_mobile_payload_t payload =
        authorization_mobile_try_verify(token);
    free(token);
    if (!payload) {
        ws_reject(res, 401, "Authentication rejected.");
        return CWS_ERR_INVALID;
    }

    ws_identity_t* id = (ws_identity_t*)calloc(1, sizeof(*id));
    if (!id) {
        jwt_mobile_payload_free(payload);
        return CWS_ERR_NOMEM;
    }
    id->usu_id = payload->user.usu_id;
    id->nombre = strdup(payload->user.usu_nombre ? payload->user.usu_nombre
                                                 : "");
    jwt_mobile_payload_free(payload);
    if (!id->nombre) {
        free(id);
        return CWS_ERR_NOMEM;
    }
    cws_ws_socket_set_data(s, id);

    /* usu_id es interno: solo se registra en el log del servidor. */
    cws_log_info("ws %s: upgrade autorizado (usu_id=%lld)", WS_V1_PATH,
                 (long long)id->usu_id);
    return CWS_OK;
}

/* Conexión establecida: une a lobby y a user:<usu_id> (interno) y manda
 * `welcome` con la identidad pública. */
static void ws_open(cws_ws_socket_t* s, void* ud) {
    (void)ud;
    ws_identity_t* id = (ws_identity_t*)cws_ws_socket_data(s);

    cws_ws_join(s, WS_V1_LOBBY);
    if (id) {
        char room[48];
        snprintf(room, sizeof(room), "user:%lld", (long long)id->usu_id);
        cws_ws_join(s, room); /* sala privada interna, no se emite el id */
    }

    char ident[1024];
    identity_json(s, id, ident, sizeof(ident));
    char payload[1280];
    int n = snprintf(payload, sizeof(payload),
                     "{\"socket_id\":\"%s\",\"path\":\"%s\",\"ts\":%lld,"
                     "\"identity\":%s}",
                     cws_ws_socket_id(s), WS_V1_PATH, (long long)time(NULL),
                     ident);
    if (n > 0 && (size_t)n < sizeof(payload))
        cws_ws_emit(s, "welcome", payload, (size_t)n);
    else
        cws_ws_emit(s, "welcome", "{}", 2);

    cws_log_info("ws open id=%s usuario=%s", cws_ws_socket_id(s),
                 id ? id->nombre : "?");
}

static void ws_close(cws_ws_socket_t* s, int code, const char* reason,
                     size_t reason_len, void* ud) {
    (void)reason;
    (void)reason_len;
    (void)ud;
    ws_identity_t* id = (ws_identity_t*)cws_ws_socket_data(s);
    cws_log_info("ws close id=%s code=%d", cws_ws_socket_id(s), code);
    if (id) {
        free(id->nombre);
        free(id);
        cws_ws_socket_set_data(s, NULL);
    }
}

static void ws_error(cws_ws_socket_t* s, int err, const char* msg, void* ud) {
    (void)ud;
    cws_log_warn("ws error id=%s err=%d msg=%s", cws_ws_socket_id(s), err,
                 msg ? msg : "");
}

/* ping <payload> -> pong <payload> */
static void ws_on_ping(cws_ws_socket_t* s, const char* event,
                       const void* data, size_t len, void* ud) {
    (void)event;
    (void)ud;
    cws_ws_emit(s, "pong", data ? data : "", len);
}

/* echo <payload> -> echo <payload> */
static void ws_on_echo(cws_ws_socket_t* s, const char* event,
                       const void* data, size_t len, void* ud) {
    (void)event;
    (void)ud;
    cws_ws_emit(s, "echo", data ? data : "", len);
}

/* join <room> -> joined <room> */
static void ws_on_join(cws_ws_socket_t* s, const char* event,
                       const void* data, size_t len, void* ud) {
    (void)event;
    (void)ud;
    char room[64];
    size_t n = data ? len : 0;
    if (n >= sizeof(room)) n = sizeof(room) - 1;
    if (n) memcpy(room, data, n);
    room[n] = '\0';
    if (n == 0) {
        cws_ws_emit(s, "error", "room requerido", 13);
        return;
    }
    cws_ws_join(s, room);
    cws_ws_emit(s, "joined", room, n);
}

/* leave <room> -> left <room> */
static void ws_on_leave(cws_ws_socket_t* s, const char* event,
                        const void* data, size_t len, void* ud) {
    (void)event;
    (void)ud;
    char room[64];
    size_t n = data ? len : 0;
    if (n >= sizeof(room)) n = sizeof(room) - 1;
    if (n) memcpy(room, data, n);
    room[n] = '\0';
    if (n == 0) {
        cws_ws_emit(s, "error", "room requerido", 13);
        return;
    }
    cws_ws_leave(s, room);
    cws_ws_emit(s, "left", room, n);
}

/*
 * message\n<texto>              -> emite a lobby
 * message\n<room>\n<texto>      -> emite a <room>
 *
 * El servidor compone el mensaje canónico y lo emite a TODOS los miembros de
 * la sala (incluido el emisor):
 *   {"public_id":"<socket_id>","usu_nombre":"<...>","msg":"<texto>",
 *    "room":"<room>","ts":<epoch>}
 * `usu_id` NO se incluye.
 */
static void ws_on_message(cws_ws_socket_t* s, const char* event,
                          const void* data, size_t len, void* ud) {
    (void)event;
    (void)ud;
    ws_identity_t* id = (ws_identity_t*)cws_ws_socket_data(s);
    if (!id) {
        cws_ws_emit(s, "error", "no autenticado", 14);
        return;
    }

    const char* p = (const char*)data;
    const char* room = WS_V1_LOBBY;
    char roombuf[64];
    const char* text = p;
    size_t text_len = len;

    if (p && len) {
        const char* nl = (const char*)memchr(p, '\n', len);
        if (nl) {
            size_t rl = (size_t)(nl - p);
            size_t copy = rl < sizeof(roombuf) - 1 ? rl : sizeof(roombuf) - 1;
            memcpy(roombuf, p, copy);
            roombuf[copy] = '\0';
            room = roombuf;
            text = nl + 1;
            text_len = len - rl - 1;
        }
    }

    if (text_len == 0) {
        cws_ws_emit(s, "error", "mensaje vacio", 13);
        return;
    }
    if (text_len > WS_V1_MAX_TEXT) {
        cws_ws_emit(s, "error", "mensaje demasiado largo", 23);
        return;
    }

    char* textz = (char*)malloc(text_len + 1);
    if (!textz) return;
    memcpy(textz, text, text_len);
    textz[text_len] = '\0';

    char* esc_pub  = json_escape_dup(cws_ws_socket_id(s));
    char* esc_nom  = json_escape_dup(id->nombre ? id->nombre : "");
    char* esc_msg  = json_escape_dup(textz);
    char* esc_room = json_escape_dup(room);
    free(textz);

    if (!esc_pub || !esc_nom || !esc_msg || !esc_room) {
        free(esc_pub);
        free(esc_nom);
        free(esc_msg);
        free(esc_room);
        return;
    }

    size_t cap = strlen(esc_pub) + strlen(esc_nom) + strlen(esc_msg) +
                 strlen(esc_room) + 160;
    char* json = (char*)malloc(cap);
    if (json) {
        int n = snprintf(
            json, cap,
            "{\"public_id\":%s,\"usu_nombre\":%s,\"msg\":%s,\"room\":%s,"
            "\"ts\":%lld}",
            esc_pub, esc_nom, esc_msg, esc_room, (long long)time(NULL));
        if (n > 0 && (size_t)n < cap && g_ws)
            cws_ws_to(g_ws, room, "message", json, (size_t)n);
        free(json);
    }
    free(esc_pub);
    free(esc_nom);
    free(esc_msg);
    free(esc_room);
}

/* delivery.ack {event_id, device_id?} -> cancela el push pendiente. */
static void ws_on_delivery_ack(cws_ws_socket_t* s, const char* event,
                              const void* data, size_t len, void* ud) {
    (void)event; (void)ud;
    ws_identity_t* id = (ws_identity_t*)cws_ws_socket_data(s);
    if (!id || !data || len == 0 || len > 2048) return;
    char* z = (char*)malloc(len + 1);
    if (!z) return;
    memcpy(z, data, len);
    z[len] = '\0';
    char* event_id = NULL;
    char* device_id = NULL;
    jwt_json_get_string(z, "event_id", &event_id);
    jwt_json_get_string(z, "device_id", &device_id);
    if (event_id && *event_id) {
        int ok = push_outbox_cancel(id->usu_id, event_id, device_id);
        cws_log_info("ws delivery.ack event=%s device=%s cancelled=%d",
                     event_id, device_id ? device_id : "",
                     ok);
    }
    free(event_id);
    free(device_id);
    free(z);
}

/* whoami -> identity <json> */
static void ws_on_whoami(cws_ws_socket_t* s, const char* event,
                         const void* data, size_t len, void* ud) {
    (void)event;
    (void)data;
    (void)len;
    (void)ud;
    ws_identity_t* id = (ws_identity_t*)cws_ws_socket_data(s);
    char ident[1024];
    identity_json(s, id, ident, sizeof(ident));
    cws_ws_emit(s, "identity", ident, strlen(ident));
}

/* stats -> stats <json> */
static void ws_on_stats(cws_ws_socket_t* s, const char* event,
                        const void* data, size_t len, void* ud) {
    (void)event;
    (void)data;
    (void)len;
    (void)ud;
    if (!g_ws) return;
    cws_ws_stats_t st;
    memset(&st, 0, sizeof(st));
    cws_ws_stats(g_ws, &st);
    char buf[320];
    int n = snprintf(
        buf, sizeof(buf),
        "{\"connections_active\":%llu,\"connections_total\":%llu,"
        "\"messages_in\":%llu,\"messages_out\":%llu,\"bytes_in\":%llu,"
        "\"bytes_out\":%llu,\"closes_total\":%llu}",
        (unsigned long long)st.connections_active,
        (unsigned long long)st.connections_total,
        (unsigned long long)st.messages_in,
        (unsigned long long)st.messages_out,
        (unsigned long long)st.bytes_in, (unsigned long long)st.bytes_out,
        (unsigned long long)st.closes_total);
    if (n > 0 && (size_t)n < sizeof(buf))
        cws_ws_emit(s, "stats", buf, (size_t)n);
}

/* Fallback: mensaje/evento sin handler conocido. */
static void ws_on_raw_message(cws_ws_socket_t* s, const void* data, size_t len,
                              int is_binary, void* ud) {
    (void)data;
    (void)len;
    (void)is_binary;
    (void)ud;
    cws_ws_emit(s, "error", "evento desconocido", 19);
}

/* --- init ---------------------------------------------------------------- */

int ws_v1_init(cws_app_t* app) {
    if (!app) return CWS_ERR_INVALID;
    if (g_ws) return CWS_OK; /* idempotente */

    /* La lib lee sus límites de WS_* al crear el hub; le damos el .env. */
    cws_app_env_file(app, ".env");

    cws_app_ws(app, WS_V1_PATH, ws_upgrade, ws_open, NULL);
    g_ws = cws_app_websocket(app);
    if (!g_ws) return CWS_ERR_GENERIC;

    cws_ws_on(g_ws, "ping", ws_on_ping, NULL);
    cws_ws_on(g_ws, "echo", ws_on_echo, NULL);
    cws_ws_on(g_ws, "join", ws_on_join, NULL);
    cws_ws_on(g_ws, "leave", ws_on_leave, NULL);
    cws_ws_on(g_ws, "message", ws_on_message, NULL);
    cws_ws_on(g_ws, "whoami", ws_on_whoami, NULL);
    cws_ws_on(g_ws, "delivery.ack", ws_on_delivery_ack, NULL);
    cws_ws_on(g_ws, "stats", ws_on_stats, NULL);
    cws_ws_on_message(g_ws, ws_on_raw_message, NULL);
    cws_ws_on_close(g_ws, ws_close, NULL);
    cws_ws_on_error(g_ws, ws_error, NULL);

    cws_log_info("ws v1: endpoint %s (auth Bearer obligatoria, GOST)",
                 WS_V1_PATH);
    return CWS_OK;
}

cws_ws_t* ws_v1_hub(void) {
    return g_ws;
}

const char* ws_v1_path(void) {
    return WS_V1_PATH;
}

int ws_v1_emit_to_user(int64_t usu_id, const char* event,
                       const void* payload, size_t len) {
    if (!g_ws || !event) return CWS_ERR_INVALID;
    char room[48];
    int n = snprintf(room, sizeof(room), "user:%lld", (long long)usu_id);
    if (n < 0 || (size_t)n >= sizeof(room)) return CWS_ERR_INVALID;
    return cws_ws_to(g_ws, room, event, payload, len);
}

int ws_v1_user_online(int64_t usu_id) {
    if (!g_ws) return 0;
    char room[48];
    int n = snprintf(room, sizeof(room), "user:%lld", (long long)usu_id);
    if (n < 0 || (size_t)n >= sizeof(room)) return 0;
    return cws_ws_room_size(g_ws, room) > 0;
}
