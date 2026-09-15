/*
 * authorization_mobile.c — autenticación para Android sin cookies.
 * Sobre core/jwt_mobile (sin IP check) y core/sql_eject.
 */

#define _POSIX_C_SOURCE 200809L

#include "authorization_mobile.h"

#include "password.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

struct authorization_mobile {
    jwt_mobile_t*  jwt;
    sql_eject_t*   sql;
};

static authorization_mobile_t* g_auth = NULL;

/* ------------------------------------------------------------------ */
/* helpers de respuesta y extracción de headers                        */
/* ------------------------------------------------------------------ */

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
                } else out[o++] = (char)*p;
                break;
        }
    }
    out[o] = '\0';
    return o;
}

static void send_json(cws_response_t* res, const char* body) {
    cws_response_body(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

static void send_json_error(cws_response_t* res, int status,
                            const char* msg) {
    char body[512];
    int n = snprintf(body, sizeof(body),
                     "{\"success\":false,\"error\":1,\"msg\":\"");
    if (n < 0) n = 0;
    size_t len = (size_t)n;
    len += json_escape_to(body + len, sizeof(body) - len - 8, msg);
    n = snprintf(body + len, sizeof(body) - len, "\"}");
    if (n < 0) len = 0; else len += (size_t)n;
    cws_response_status(res, status);
    cws_response_body(res, body, len, CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

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
    while (*p == ' ') p++;
    /* case-insensitive "Bearer " */
    if (strncasecmp(p, "Bearer", 6) != 0) { free(h); return NULL; }
    p += 6;
    while (*p == ' ') p++;
    char* tok = strdup(p);
    /* limpiar CR/LF finales por seguridad */
    if (tok) {
        size_t l = strlen(tok);
        while (l > 0 && (tok[l-1] == '\r' || tok[l-1] == '\n' || tok[l-1] == ' '))
            tok[--l] = '\0';
    }
    free(h);
    return tok;
}

/* IP del peer de la conexión (getpeername). No se valida en mobile, pero
 * se copia al payload.user.ip al emitir tokens para auditoría. */
static char* client_ip(const cws_response_t* res) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (!res || getpeername(res->client_fd, (struct sockaddr*)&ss, &sl) != 0)
        return NULL;
    char ip[INET6_ADDRSTRLEN] = "?";
    if (ss.ss_family == AF_INET)
        inet_ntop(AF_INET, &((struct sockaddr_in*)&ss)->sin_addr, ip,
                  sizeof ip);
    else if (ss.ss_family == AF_INET6)
        inet_ntop(AF_INET6, &((struct sockaddr_in6*)&ss)->sin6_addr, ip,
                  sizeof ip);
    return strdup(ip);
}

/* trim en sitio. */
static char* trim_inplace(char* s) {
    if (!s) return s;
    while (*s == ' ' || *s == '\t') s++;
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t')) s[--len] = '\0';
    return s;
}

/* ------------------------------------------------------------------ */
/* ciclo de vida                                                       */
/* ------------------------------------------------------------------ */

authorization_mobile_t* authorization_mobile_new(const app_config_t* cfg) {
    if (!cfg) return NULL;
    authorization_mobile_t* auth = calloc(1, sizeof(*auth));
    if (!auth) return NULL;
    auth->jwt = jwt_mobile_new(cfg);
    auth->sql = sql_eject_new(cfg);
    if (!auth->jwt || !auth->sql) {
        authorization_mobile_free(auth);
        return NULL;
    }
    return auth;
}

void authorization_mobile_free(authorization_mobile_t* auth) {
    if (!auth) return;
    jwt_mobile_free(auth->jwt);
    sql_eject_free(auth->sql);
    free(auth);
}

void authorization_mobile_init(void) {
    app_config_t* cfg = configuration_new();
    if (!cfg) return;
    authorization_mobile_t* auth = authorization_mobile_new(cfg);
    configuration_free(cfg);
    if (!auth) return;
    authorization_mobile_free(g_auth);
    g_auth = auth;
}

void authorization_mobile_shutdown(void) {
    authorization_mobile_free(g_auth);
    g_auth = NULL;
}

/* ------------------------------------------------------------------ */
/* DAO                                                                 */
/* ------------------------------------------------------------------ */

sql_result_t* authorization_mobile_user_dao(authorization_mobile_t* auth,
                                           const char* usu_correo) {
    if (!auth || !usu_correo || !*usu_correo) return NULL;
    sql_param_t params[2];
    params[0].name = "tipoConsulta";
    params[0].type = SQL_PT_INT;
    params[0].val.as_int = 1;     /* CONS_USU_SIGNIN */
    params[1].name = "usu_correo";
    params[1].type = SQL_PT_STRING;
    params[1].val.as_string = usu_correo;
    sql_result_t* out = calloc(1, sizeof(*out));
    if (!out) return NULL;
    if (sql_eject_store(auth->sql, "procUsersCons", AUTH_MOBILE_DB, params, 2,
                         out) != CWS_OK) {
        sql_result_free(out);
        return NULL;
    }
    return out;
}

static const sql_value_t* dao_field(const sql_result_t* res, const char* name) {
    if (!res || res->nrows == 0) return NULL;
    for (size_t c = 0; c < res->ncols; c++) {
        if (strcasecmp(res->columns[c].name, name) == 0)
            return &res->rows[0].cols[c];
    }
    return NULL;
}

static int64_t dao_int(const sql_result_t* res, const char* name, int64_t def) {
    const sql_value_t* v = dao_field(res, name);
    return (v && !v->is_null) ? v->val.as_int : def;
}
static char* dao_str(const sql_result_t* res, const char* name) {
    const sql_value_t* v = dao_field(res, name);
    if (!v || v->is_null || !v->val.as_string) return NULL;
    return strdup(v->val.as_string);
}

/* ------------------------------------------------------------------ */
/* estado de la petición                                               */
/* ------------------------------------------------------------------ */

typedef struct mobile_ctx {
    int                  authorized;
    jwt_mobile_payload_t payload;
} mobile_ctx_t;

const jwt_user_t* authorization_mobile_request_user(const cws_request_t* req) {
    const mobile_ctx_t* ctx =
        req ? (const mobile_ctx_t*)req->__user : NULL;
    return (ctx && ctx->authorized) ? &ctx->payload->user : NULL;
}

bool authorization_mobile_request_authorized(const cws_request_t* req) {
    const mobile_ctx_t* ctx =
        req ? (const mobile_ctx_t*)req->__user : NULL;
    return ctx && ctx->authorized;
}

/* Valida el payload contra el DAO: usu_correo existe y usu_salt coincide. */
static int authorize_mobile(authorization_mobile_t* auth,
                            cws_request_t* req, cws_response_t* res,
                            jwt_mobile_payload_t payload) {
    /* El DAO indexa el correo por su hash (igual que signin). */
    char* hashed = jwt_core_hash_hex(payload->user.usu_correo);
    sql_result_t* dao = hashed ? authorization_mobile_user_dao(auth, hashed)
                               : NULL;
    free(hashed);
    if (!dao || dao->nrows == 0) {
        sql_result_free(dao);
        send_json_error(res, 500, "Error al iniciar sesión");
        return 0;
    }
    char* dao_salt = dao_str(dao, "usu_salt");
    sql_result_free(dao);
    int salt_ok = 0;
    if (payload->user.usu_salt && dao_salt) {
        char* tok_salt = strdup(payload->user.usu_salt);
        char* db_salt = strdup(dao_salt);
        if (tok_salt && db_salt)
            salt_ok = jwt_safe_compare(trim_inplace(tok_salt),
                                      trim_inplace(db_salt));
        free(tok_salt);
        free(db_salt);
    }
    free(dao_salt);
    if (!salt_ok) {
        send_json_error(res, 401, "Error al iniciar sesión");
        return 0;
    }
    mobile_ctx_t* ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { send_json_error(res, 500, "Error al iniciar sesión"); return 0; }
    ctx->authorized = 1;
    ctx->payload = payload;
    req->__user = ctx;
    return 1;
}

static void free_mobile_ctx(cws_request_t* req) {
    mobile_ctx_t* ctx = (mobile_ctx_t*)req->__user;
    if (ctx) {
        jwt_mobile_payload_free(ctx->payload);
        free(ctx);
        req->__user = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* middlewares                                                        */
/* ------------------------------------------------------------------ */

void cws_mw_authorization_mobile_verify(cws_request_t* req, cws_response_t* res,
                                        cws_next_fn next) {
    if (!g_auth) {
        send_json_error(res, 500, "Servicio no inicializado");
        return;
    }
    char* token = bearer_token(req);
    if (!token || !*token) {
        send_json_error(res, 401, "No credentials are present.");
        free(token);
        return;
    }
    jwt_mobile_payload_t payload =
        jwt_mobile_verify_access_token(g_auth->jwt, token);
    free(token);
    if (!payload) {
        send_json_error(res, 401, "Authentication rejected.");
        return;
    }
    if (!authorize_mobile(g_auth, req, res, payload)) {
        jwt_mobile_payload_free(payload);
        free_mobile_ctx(req);
        return;
    }
    next(req, res);
    free_mobile_ctx(req);
}

void authorization_mobile_refresh(cws_request_t* req, cws_response_t* res) {
    if (!g_auth) {
        send_json_error(res, 500, "Servicio no inicializado");
        return;
    }
    char* token = header_dup(req, "x-refresh-token");
    if (!token || !*token) {
        send_json_error(res, 401, "No credentials are present.");
        free(token);
        return;
    }
    jwt_mobile_payload_t payload =
        jwt_mobile_verify_refresh_token(g_auth->jwt, token);
    free(token);
    if (!payload) {
        send_json_error(res, 401, "Authentication rejected.");
        return;
    }
    if (!authorize_mobile(g_auth, req, res, payload)) {
        jwt_mobile_payload_free(payload);
        free_mobile_ctx(req);
        return;
    }

    char* ip = client_ip(res);
    jwt_user_t user = payload->user;
    user.ip = ip;
    char* access = jwt_mobile_write_access_token(g_auth->jwt, &user);
    free(ip);

    char body[1700];
    int n;
    if (access) {
        n = snprintf(body, sizeof(body),
                     "{\"access_token\":\"%s\",\"token_type\":\"Bearer\"}",
                     access);
    } else {
        n = snprintf(body, sizeof(body),
                     "{\"error\":\"token_generation_failed\"}");
    }
    if (n < 0 || (size_t)n >= sizeof(body)) {
        cws_response_send_error(res, 500);
    } else {
        cws_response_status(res, 200);
        send_json(res, body);
    }
    free(access);
    free_mobile_ctx(req);
}

/* ------------------------------------------------------------------ */
/* signin                                                              */
/* ------------------------------------------------------------------ */

static char* body_dup(const cws_request_t* req) {
    if (!req || !req->body || req->body_len == 0) return NULL;
    size_t len = req->body_len > 8192 ? 8192 : req->body_len;
    char* out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, req->body, len);
    out[len] = '\0';
    return out;
}

int authorization_mobile_signin(cws_request_t* req, cws_response_t* res) {
    if (!g_auth || !req) return CWS_ERR_INVALID;

    char* body = body_dup(req);
    char* email = NULL;
    char* password = NULL;
    if (body) {
        jwt_json_get_string(body, "usu_correo", &email);
        jwt_json_get_string(body, "usu_contrasena", &password);
    }
    if (!email || !*email || !password || !*password) {
        send_json_error(res, 400, "Campos requeridos: correo, contraseña");
        free(body); free(email); free(password);
        return CWS_ERR_NOTFOUND;
    }
    trim_inplace(email);
    for (char* p = email; *p; p++) *p = (char)tolower((unsigned char)*p);

    char* hashed = jwt_core_hash_hex(email);
    if (!hashed) { free(body); free(email); free(password); return CWS_ERR_GENERIC; }

    sql_result_t* dao = authorization_mobile_user_dao(g_auth, hashed);
    free(hashed);
    if (!dao || dao->nrows == 0) {
        sql_result_free(dao);
        send_json_error(res, 401, "El usuario no fue autorizado o no existe.");
        free(body); free(email); free(password);
        return CWS_ERR_NOTFOUND;
    }
    char* stored_hash = dao_str(dao, "usu_contrasena");
    int64_t usu_id = dao_int(dao, "usu_id", 0);
    char* usu_nombre = dao_str(dao, "usu_nombre");
    char* usu_salt = dao_str(dao, "usu_salt");
    sql_result_free(dao);

    password_result_t pr = password_verify(password, stored_hash ? stored_hash : "");
    if (pr == PASSWORD_OK_LEGACY)
        cws_log_warn("mobile signin: hash legacy de usu_id=%lld (conviene rehashear)",
                     (long long)usu_id);
    if (pr != PASSWORD_OK && pr != PASSWORD_OK_LEGACY) {
        send_json_error(res, 401, "El usuario no fue autorizado o no existe.");
        free(stored_hash); free(usu_nombre); free(usu_salt);
        free(body); free(email); free(password);
        return CWS_ERR_NOTFOUND;
    }

    char* ip = client_ip(res);
    jwt_user_t user = {
        .usu_id = usu_id,
        .usu_nombre = usu_nombre,
        .usu_correo = strdup(email),
        .ip = ip,
        .usu_salt = strdup(usu_salt ? usu_salt : ""),
    };
    char* access = jwt_mobile_write_access_token(g_auth->jwt, &user);
    char* refresh = jwt_mobile_write_refresh_token(g_auth->jwt, &user);
    free(ip);

    /* Body con ambos tokens. */
    char body_json[2200];
    int n = snprintf(body_json, sizeof(body_json),
                     "{\"access_token\":\"%s\","
                     "\"refresh_token\":\"%s\","
                     "\"token_type\":\"Bearer\","
                     "\"user\":{\"usu_id\":%lld,\"usu_nombre\":\"%s\","
                     "\"usu_correo\":\"%s\"}}",
                     access ? access : "",
                     refresh ? refresh : "",
                     (long long)usu_id,
                     usu_nombre ? usu_nombre : "",
                     email ? email : "");
    if (n < 0 || (size_t)n >= sizeof(body_json)) {
        cws_response_send_error(res, 500);
    } else {
        cws_response_status(res, 200);
        send_json(res, body_json);
    }
    free(access);
    free(refresh);
    free(user.usu_correo);
    free(user.usu_salt);
    free(stored_hash);
    free(usu_nombre);
    free(usu_salt);
    free(body);
    free(email);
    free(password);
    return CWS_OK;
}
