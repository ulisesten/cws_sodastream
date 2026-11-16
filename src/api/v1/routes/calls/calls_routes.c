#define _POSIX_C_SOURCE 200809L

#include "calls_routes.h"

#include <stdarg.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "authorization_mobile.h"
#include "configuration.h"
#include "jwt.h"
#include "integrations/telegram/telegram_calls.h"
#include "ws_api/v1/ws_v1.h"

#define CALL_BODY_MAX (256u * 1024u)
#define CALL_ID_MAX   64

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} sb_t;

static int sb_reserve(sb_t* b, size_t extra) {
    if (!b) return -1;
    if (b->len + extra + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 512;
    while (cap < b->len + extra + 1) cap *= 2;
    char* p = (char*)realloc(b->data, cap);
    if (!p) return -1;
    b->data = p;
    b->cap = cap;
    return 0;
}

static int sb_puts(sb_t* b, const char* s) {
    if (!s) s = "";
    size_t n = strlen(s);
    if (sb_reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int sb_printf(sb_t* b, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[1024];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof(tmp)) return sb_puts(b, tmp);
    char* big = (char*)malloc((size_t)n + 1);
    if (!big) return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int rc = sb_puts(b, big);
    free(big);
    return rc;
}

static void sb_free(sb_t* b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

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

static char* json_escape_alloc(const char* s) {
    size_t n = strlen(s ? s : "");
    char* out = (char*)malloc(n * 6 + 1);
    if (!out) return NULL;
    json_escape(out, n * 6 + 1, s);
    return out;
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

static char* body_dup(const cws_request_t* req) {
    if (!req || !req->body || req->body_len == 0) return NULL;
    if (req->body_len > CALL_BODY_MAX) return NULL;
    char* b = (char*)malloc(req->body_len + 1);
    if (!b) return NULL;
    memcpy(b, req->body, req->body_len);
    b[req->body_len] = '\0';
    return b;
}

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

static int valid_call_id(const char* s) {
    if (!s) return 0;
    size_t n = strlen(s);
    if (n < 6 || n > CALL_ID_MAX) return 0;
    for (const char* p = s; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')
            continue;
        return 0;
    }
    return 1;
}

static int is_participant(const telegram_call_t* c, int64_t uid) {
    return c && (c->caller_uid == uid || c->callee_uid == uid);
}

static int64_t peer_uid(const telegram_call_t* c, int64_t uid) {
    if (!c) return 0;
    return c->caller_uid == uid ? c->callee_uid : c->caller_uid;
}

static const char* self_public(const telegram_call_t* c, int64_t uid) {
    if (!c) return "";
    return c->caller_uid == uid ? c->caller_public : c->callee_public;
}

static const char* peer_public(const telegram_call_t* c, int64_t uid) {
    if (!c) return "";
    return c->caller_uid == uid ? c->callee_public : c->caller_public;
}

static const char* peer_name(const telegram_call_t* c, int64_t uid) {
    if (!c) return "";
    return c->caller_uid == uid ? c->callee_name : c->caller_name;
}

static const char* peer_ip(const telegram_call_t* c, int64_t uid) {
    if (!c) return "";
    return c->caller_uid == uid ? c->callee_ip : c->caller_ip;
}

static const char* state_name(int st) {
    switch (st) {
        case 0: return "ringing";
        case 1: return "accepted";
        case 2: return "ended";
        case 3: return "rejected";
        default: return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/* ICE / TURN                                                          */
/* ------------------------------------------------------------------ */

static void append_stun(sb_t* b, int* first, const char* url) {
    char* e = json_escape_alloc(url);
    if (!e) return;
    sb_printf(b, "%s{\"urls\":[\"%s\"]}", *first ? "" : ",", e);
    *first = 0;
    free(e);
}

static void append_turn(sb_t* b, int* first, const char* url,
                        const char* user, const char* cred) {
    char* eu = json_escape_alloc(url);
    char* uu = json_escape_alloc(user);
    char* cu = json_escape_alloc(cred);
    if (eu && uu && cu)
        sb_printf(b, "%s{\"urls\":[\"%s\"],\"username\":\"%s\","
                     "\"credential\":\"%s\"}",
                  *first ? "" : ",", eu, uu, cu);
    free(eu); free(uu); free(cu);
    *first = 0;
}

static int turn_credential(const char* public_id, char* user, size_t ucap,
                           char* cred, size_t ccap) {
    const char* secret = cfg_getenv("TURN_SECRET");
    if (!secret || !*secret) return 0;
    int ttl = 3600;
    const char* t = cfg_getenv("TURN_TTL_SECONDS");
    if (t && *t) ttl = atoi(t);
    if (ttl < 60) ttl = 60;
    if (ttl > 86400) ttl = 86400;

    time_t exp = time(NULL) + ttl;
    const char* id = (public_id && *public_id) ? public_id : "anon";
    snprintf(user, ucap, "%lld:%s", (long long)exp, id);

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    if (!HMAC(EVP_sha1(), secret, (int)strlen(secret),
              (const unsigned char*)user, strlen(user), digest, &dlen))
        return 0;
    int n = EVP_EncodeBlock((unsigned char*)cred, digest, (int)dlen);
    if (n <= 0 || (size_t)n >= ccap) return 0;
    cred[n] = '\0';
    return ttl;
}

static void append_urls(sb_t* b, int* first, const char* csv) {
    if (!csv || !*csv) return;
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", csv);
    char* save = NULL;
    for (char* p = strtok_r(tmp, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
        while (*p == ' ' || *p == '\t') p++;
        size_t n = strlen(p);
        while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) p[--n] = '\0';
        if (!*p) continue;
        if (strncasecmp(p, "turn:", 5) == 0 ||
            strncasecmp(p, "turns:", 6) == 0) {
            /* handled below with credentials */
            append_stun(b, first, p); /* placeholder overwritten below? */
        } else {
            append_stun(b, first, p);
        }
    }
}

/* ------------------------------------------------------------------ */
/* GET /:call_id/ice                                                  */
/* ------------------------------------------------------------------ */
static CWS_HANDLER(calls_ice_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { send_error(res, 401, "No autorizado"); return; }
    char* call_id = param_dup(req, "call_id");
    if (!valid_call_id(call_id)) {
        free(call_id);
        send_error(res, 400, "call_id inválido");
        return;
    }
    telegram_call_t call;
    memset(&call, 0, sizeof(call));
    if (!telegram_calls_get(call_id, &call) || !is_participant(&call, u->usu_id) ||
        call.state > 1) {
        free(call_id);
        send_error(res, 404, "Llamada no encontrada o finalizada");
        return;
    }

    const char* pub = self_public(&call, u->usu_id);
    char user[128] = "", cred[128] = "";
    int ttl = turn_credential(pub, user, sizeof(user), cred, sizeof(cred));

    sb_t b = {0};
    sb_puts(&b, "{\"success\":true,\"error\":0,\"data\":{\"ice_servers\":[");
    int first = 1;
    append_urls(&b, &first, cfg_getenv("STUN_URLS"));

    const char* turn = cfg_getenv("TURN_URLS");
    if (ttl > 0 && turn && *turn) {
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s", turn);
        char* save = NULL;
        for (char* p = strtok_r(tmp, ",", &save); p;
             p = strtok_r(NULL, ",", &save)) {
            while (*p == ' ' || *p == '\t') p++;
            size_t n = strlen(p);
            while (n && (p[n - 1] == ' ' || p[n - 1] == '\t')) p[--n] = '\0';
            if (*p) append_turn(&b, &first, p, user, cred);
        }
    }
    const char* stun_cfg = cfg_getenv("STUN_URLS");
    const char* turn_cfg = cfg_getenv("TURN_URLS");
    const char* turn_sec = cfg_getenv("TURN_SECRET");
    int have_stun = stun_cfg && *stun_cfg;
    int have_turn = turn_cfg && *turn_cfg && turn_sec && *turn_sec;
    const char* warning = NULL;
    if (!have_stun && !have_turn)
        warning = "STUN/TURN no configurados: ice_servers vacío";
    else if (!have_turn)
        warning = "TURN no configurado: solo STUN";
    sb_printf(&b, "],\"expires_in\":%d", ttl > 0 ? ttl : 0);
    if (warning) sb_printf(&b, ",\"warning\":\"%s\"", warning);
    sb_puts(&b, "}}");
    free(call_id);
    if (!b.data) { send_error(res, 500, "Sin memoria"); return; }
    cws_response_status(res, 200);
    cws_response_body_owned(res, b.data, b.len, CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

/* ------------------------------------------------------------------ */
/* GET /:call_id                                                      */
/* ------------------------------------------------------------------ */
static CWS_HANDLER(calls_get_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { send_error(res, 401, "No autorizado"); return; }
    char* call_id = param_dup(req, "call_id");
    if (!valid_call_id(call_id)) {
        free(call_id);
        send_error(res, 400, "call_id inválido");
        return;
    }
    telegram_call_t call;
    memset(&call, 0, sizeof(call));
    if (!telegram_calls_get(call_id, &call) || !is_participant(&call, u->usu_id)) {
        free(call_id);
        send_error(res, 404, "Llamada no encontrada");
        return;
    }
    char peer_pub[TC_PUBLIC_MAX], peer_n[TC_NAME_MAX], ip[TC_IP_MAX];
    json_escape(peer_pub, sizeof(peer_pub), peer_public(&call, u->usu_id));
    json_escape(peer_n, sizeof(peer_n), peer_name(&call, u->usu_id));
    json_escape(ip, sizeof(ip), peer_ip(&call, u->usu_id));
    char body[1024];
    snprintf(body, sizeof(body),
             "{\"success\":true,\"error\":0,\"data\":{"
             "\"call_id\":\"%s\",\"tipo\":\"%s\",\"estado\":\"%s\","
             "\"role\":\"%s\",\"peer_public\":\"%s\",\"peer_name\":\"%s\","
             "\"peer_ip\":\"%s\",\"created\":%lld,\"updated\":%lld}}",
             call_id, call.tipo, state_name(call.state),
             call.caller_uid == u->usu_id ? "caller" : "callee",
             peer_pub, peer_n, ip,
             (long long)call.created, (long long)call.updated);
    free(call_id);
    send_json(res, 200, body);
}

/* ------------------------------------------------------------------ */
/* POST /:call_id/sdp                                                 */
/* ------------------------------------------------------------------ */
static CWS_HANDLER(calls_sdp_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { send_error(res, 401, "No autorizado"); return; }
    char* call_id = param_dup(req, "call_id");
    if (!valid_call_id(call_id)) {
        free(call_id);
        send_error(res, 400, "call_id inválido");
        return;
    }
    telegram_call_t call;
    memset(&call, 0, sizeof(call));
    if (!telegram_calls_get(call_id, &call) || !is_participant(&call, u->usu_id)) {
        free(call_id);
        send_error(res, 404, "Llamada no encontrada");
        return;
    }
    if (call.state != 1) {
        free(call_id);
        send_error(res, 409, "La llamada no está aceptada");
        return;
    }

    char* body = body_dup(req);
    char* type = NULL;
    char* sdp = NULL;
    if (body) {
        jwt_json_get_string(body, "type", &type);
        jwt_json_get_string(body, "sdp", &sdp);
    }
    if (!type || (strcmp(type, "offer") != 0 && strcmp(type, "answer") != 0) ||
        !sdp || strlen(sdp) < 10) {
        free(body); free(type); free(sdp); free(call_id);
        send_error(res, 400, "type (offer/answer) y sdp son requeridos");
        return;
    }

    char* from_pub = json_escape_alloc(self_public(&call, u->usu_id));
    char* type_e = json_escape_alloc(type);
    char* sdp_e = json_escape_alloc(sdp);
    if (!from_pub || !type_e || !sdp_e) {
        free(from_pub); free(type_e); free(sdp_e);
        free(body); free(type); free(sdp); free(call_id);
        send_error(res, 500, "Sin memoria");
        return;
    }
    size_t plen = strlen(from_pub) + strlen(type_e) + strlen(sdp_e) + 256;
    char* payload = (char*)malloc(plen);
    if (!payload) {
        free(from_pub); free(type_e); free(sdp_e);
        free(body); free(type); free(sdp); free(call_id);
        send_error(res, 500, "Sin memoria");
        return;
    }
    snprintf(payload, plen,
             "{\"call_id\":\"%s\",\"from_public\":\"%s\","
             "\"type\":\"%s\",\"sdp\":\"%s\"}",
             call_id, from_pub, type_e, sdp_e);
    int64_t peer = peer_uid(&call, u->usu_id);
    ws_v1_emit_to_user(peer, "call.sdp", payload, strlen(payload));

    free(payload); free(from_pub); free(type_e); free(sdp_e);
    free(body); free(type); free(sdp); free(call_id);
    send_json(res, 200,
              "{\"success\":true,\"error\":0,\"data\":{\"relayed\":true}}");
}

/* ------------------------------------------------------------------ */
/* POST /:call_id/ice                                                 */
/* ------------------------------------------------------------------ */
static CWS_HANDLER(calls_ice_candidate_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { send_error(res, 401, "No autorizado"); return; }
    char* call_id = param_dup(req, "call_id");
    if (!valid_call_id(call_id)) {
        free(call_id);
        send_error(res, 400, "call_id inválido");
        return;
    }
    telegram_call_t call;
    memset(&call, 0, sizeof(call));
    if (!telegram_calls_get(call_id, &call) || !is_participant(&call, u->usu_id)) {
        free(call_id);
        send_error(res, 404, "Llamada no encontrada");
        return;
    }
    if (call.state != 1) {
        free(call_id);
        send_error(res, 409, "La llamada no está aceptada");
        return;
    }

    char* body = body_dup(req);
    char* cand = NULL;
    char* mid = NULL;
    char* ufrag = NULL;
    int64_t idx = 0;
    if (body) {
        jwt_json_get_string(body, "candidate", &cand);
        jwt_json_get_string(body, "sdpMid", &mid);
        jwt_json_get_string(body, "usernameFragment", &ufrag);
        jwt_json_get_int(body, "sdpMLineIndex", &idx);
    }
    if (!cand || !*cand) {
        free(body); free(cand); free(mid); free(ufrag); free(call_id);
        send_error(res, 400, "candidate es requerido");
        return;
    }

    char* from_pub = json_escape_alloc(self_public(&call, u->usu_id));
    char* cand_e = json_escape_alloc(cand);
    char* mid_e = json_escape_alloc(mid ? mid : "");
    char* uf_e = json_escape_alloc(ufrag ? ufrag : "");
    if (!from_pub || !cand_e || !mid_e || !uf_e) {
        free(from_pub); free(cand_e); free(mid_e); free(uf_e);
        free(body); free(cand); free(mid); free(ufrag); free(call_id);
        send_error(res, 500, "Sin memoria");
        return;
    }
    size_t plen = strlen(from_pub) + strlen(cand_e) + strlen(mid_e) +
                  strlen(uf_e) + 320;
    char* payload = (char*)malloc(plen);
    if (!payload) {
        free(from_pub); free(cand_e); free(mid_e); free(uf_e);
        free(body); free(cand); free(mid); free(ufrag); free(call_id);
        send_error(res, 500, "Sin memoria");
        return;
    }
    snprintf(payload, plen,
             "{\"call_id\":\"%s\",\"from_public\":\"%s\","
             "\"candidate\":\"%s\",\"sdpMid\":\"%s\","
             "\"sdpMLineIndex\":%lld,\"usernameFragment\":\"%s\"}",
             call_id, from_pub, cand_e, mid_e, (long long)idx, uf_e);
    int64_t peer = peer_uid(&call, u->usu_id);
    ws_v1_emit_to_user(peer, "call.ice", payload, strlen(payload));

    free(payload); free(from_pub); free(cand_e); free(mid_e); free(uf_e);
    free(body); free(cand); free(mid); free(ufrag); free(call_id);
    send_json(res, 200,
              "{\"success\":true,\"error\":0,\"data\":{\"relayed\":true}}");
}

/* GET /:call_id/ice (credenciales) y POST /:call_id/ice (candidate)
 * comparten path; cws guarda un método por path, así que se despacha. */
static CWS_HANDLER(calls_ice_path_handler) {
    if (req->method == CWS_M_GET) {
        calls_ice_handler(req, res);
    } else if (req->method == CWS_M_POST) {
        calls_ice_candidate_handler(req, res);
    } else {
        cws_response_send_error(res, 405);
    }
}

cws_router_t* calls_routes(void) {
    const char* stun = cfg_getenv("STUN_URLS");
    const char* turn = cfg_getenv("TURN_URLS");
    const char* sec = cfg_getenv("TURN_SECRET");
    fprintf(stderr,
            "calls: ICE STUN=%s TURN=%s TURN_SECRET=%s\n",
            (stun && *stun) ? "configurado" : "vacío",
            (turn && *turn) ? "configurado" : "vacío",
            (sec && *sec) ? "configurado" : "vacío");

    cws_router_t* r = cws_router_new();
    if (!r) return NULL;
    cws_router_use(r, cws_mw_authorization_mobile_verify);
    cws_router_add(r, CWS_M_UNKNOWN, "/:call_id/ice", calls_ice_path_handler);
    cws_router_add(r, CWS_M_GET, "/:call_id", calls_get_handler);
    cws_router_add(r, CWS_M_POST, "/:call_id/sdp", calls_sdp_handler);
    return r;
}
