/*
 * authorization.c — autorización de peticiones y signin.
 *
 * Adaptación de reference/authorization.js (AuthorizationService) sobre
 * core/jwt (tokens GOST) y core/sql_eject (userDAO). El middleware valida
 * cookie access_token + header x-csrf-token, revalida el salt del DAO y
 * publica el usuario en req->__user.
 */

#define _POSIX_C_SOURCE 200809L

#include "authorization.h"

#include "password.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

struct authorization {
    jwt_t*       jwt;
    sql_eject_t* sql;
    int          is_prod;        /* NODE_ENV == "production".          */
    int          access_minutes; /* ACCESS_TOKEN_EXPIRATION_MINUTES.   */
    int          refresh_days;   /* REFRESH_TOKEN_EXPIRATION_DAYS.     */
};

static authorization_t* g_auth = NULL;

/* Emite un Set-Cookie (definido abajo; se usa en signin y refresh). */
static void set_cookie(cws_response_t* res, const char* name,
                       const char* value, const char* path,
                       int max_age_sec, int http_only, int secure);

/* ------------------------------------------------------------------ */
/* respuestas de error (reject / res.status(401).json)                 */
/* ------------------------------------------------------------------ */

static void send_json_error( cws_response_t* res, int status, const char* msg ) {
    char body[256];
    int n = snprintf(body, sizeof(body),
                     "{\"success\":false,\"error\":1,\"msg\":\"");
    if (n < 0) n = 0;
    size_t len = (size_t)n;
    /* msg no controlado: escapado mínimo (comillas y barras). */
    for (const char* p = msg; *p && len + 2 < sizeof(body); p++) {
        if (*p == '"' || *p == '\\') body[len++] = '\\';
        body[len++] = *p;
    }
    n = snprintf(body + len, sizeof(body) - len, "\",\"status\":%d}", status);
    if (n < 0) len = 0;
    else len += (size_t)n;

    cws_response_status(res, status);
    cws_response_body(res, body, len, CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

/* ------------------------------------------------------------------ */
/* extracción segura de headers/cookies (valores no NUL-terminados)     */
/* ------------------------------------------------------------------ */

/* Copia NUL-terminada del header `name` (recorre req->headers con
 * longitudes; cws_request_header no entrega longitud). malloc o NULL. */
static char* header_dup( const cws_request_t* req, const char* name ) {
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

/* Valor de la cookie `name` dentro del header Cookie (bounded). malloc o
 * NULL si no está. */
static char* cookie_value(const cws_request_t* req, const char* name) {
    char* cookie_hdr = header_dup(req, "cookie");
    if (!cookie_hdr) return NULL;

    size_t nlen = strlen(name);
    const char* p = cookie_hdr;
    char* out = NULL;
    while (*p) {
        while (*p == ' ') p++;
        const char* eq = strchr(p, '=');
        const char* semi = strchr(p, ';');
        if (!semi) semi = p + strlen(p);
        if (eq && eq < semi &&
            (size_t)(eq - p) == nlen &&
            strncasecmp(p, name, nlen) == 0) {
            size_t vlen = (size_t)(semi - eq - 1);
            out = (char*)malloc(vlen + 1);
            if (out) {
                memcpy(out, eq + 1, vlen);
                out[vlen] = '\0';
            }
            break;
        }
        p = (*semi == ';') ? semi + 1 : semi;
    }
    free(cookie_hdr);
    return out;
}

/* IP del peer de la conexión (req.ip): getpeername sobre el fd de la
 * respuesta. malloc o NULL. */
static char* client_ip(const cws_response_t* res) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (!res || getpeername(res->client_fd, (struct sockaddr*)&ss, &sl) != 0)
        return NULL;
    char ip[INET6_ADDRSTRLEN] = "?";
    if (ss.ss_family == AF_INET) {
        inet_ntop(AF_INET, &((struct sockaddr_in*)&ss)->sin_addr, ip,
                  sizeof(ip));
    } else if (ss.ss_family == AF_INET6) {
        inet_ntop(AF_INET6, &((struct sockaddr_in6*)&ss)->sin6_addr, ip,
                  sizeof(ip));
    }
    return strdup(ip);
}

/* trim en sitio (strcpy-like sobre el mismo buffer). */
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

authorization_t* authorization_new(const app_config_t* cfg) {
    if (!cfg) return NULL;
    authorization_t* auth = (authorization_t*)calloc(1, sizeof(*auth));
    if (!auth) return NULL;
    auth->jwt = jwt_new(cfg);
    auth->sql = sql_eject_new(cfg);
    auth->is_prod = cfg->is_production;
    auth->access_minutes = cfg->access_token_expiration_minutes;
    auth->refresh_days = cfg->refresh_token_expiration_days;
    if (!auth->jwt || !auth->sql) {
        authorization_free(auth);
        return NULL;
    }
    return auth;
}

void authorization_free(authorization_t* auth) {
    if (!auth) return;
    jwt_free(auth->jwt);
    sql_eject_free(auth->sql);
    free(auth);
}

int authorization_init(void) {
    app_config_t* cfg = configuration_new();
    if (!cfg) return 0;
    authorization_t* auth = authorization_new(cfg);
    configuration_free(cfg);
    if (!auth) return 0;
    authorization_free(g_auth);
    g_auth = auth;
    return 1;
}

void authorization_shutdown(void) {
    authorization_free(g_auth);
    g_auth = NULL;
}

/* ------------------------------------------------------------------ */
/* DAO                                                                 */
/* ------------------------------------------------------------------ */

sql_result_t* authorization_user_dao(authorization_t* auth,
                                     const char* usu_correo) {
    if (!auth || !usu_correo || !*usu_correo) return NULL;

    sql_param_t params[2];
    params[0].name = "tipoConsulta";
    params[0].type = SQL_PT_INT;
    params[0].val.as_int = AUTH_CONS_USU_SIGNIN;
    params[1].name = "usu_correo";
    params[1].type = SQL_PT_STRING;
    params[1].val.as_string = usu_correo;

    sql_result_t* out = (sql_result_t*)calloc(1, sizeof(*out));
    if (!out) return NULL;
    int rc = sql_eject_store(auth->sql, "procUsersCons", AUTH_DB, params, 2,
                             out);
    if (rc != CWS_OK) {
        sql_result_free(out);
        return NULL;
    }
    return out;
}

/* Campo de la primera fila por nombre de columna. NULL si no existe. */
static const sql_value_t* dao_field(const sql_result_t* res,
                                    const char* name) {
    if (!res || res->nrows == 0) return NULL;
    for (size_t c = 0; c < res->ncols; c++) {
        if (strcasecmp(res->columns[c].name, name) == 0)
            return &res->rows[0].cols[c];
    }
    return NULL;
}

static int64_t dao_int(const sql_result_t* res, const char* name,
                       int64_t def) {
    const sql_value_t* v = dao_field(res, name);
    return (v && !v->is_null) ? v->val.as_int : def;
}

/* Copia del campo string (NUL-terminada) o NULL si es NULL/vacía. */
static char* dao_str(const sql_result_t* res, const char* name) {
    const sql_value_t* v = dao_field(res, name);
    if (!v || v->is_null || !v->val.as_string) return NULL;
    return strdup(v->val.as_string);
}

/* ------------------------------------------------------------------ */
/* estado de la petición                                               */
/* ------------------------------------------------------------------ */

typedef struct auth_ctx {
    int                 authorized;
    jwt_gost_payload_t* payload;
} auth_ctx_t;

const jwt_user_t* authorization_request_user(const cws_request_t* req) {
    const auth_ctx_t* ctx =
        req ? (const auth_ctx_t*)req->__user : NULL;
    return (ctx && ctx->authorized) ? &ctx->payload->user : NULL;
}

bool authorization_request_authorized(const cws_request_t* req) {
    const auth_ctx_t* ctx =
        req ? (const auth_ctx_t*)req->__user : NULL;
    return ctx && ctx->authorized;
}

/* ------------------------------------------------------------------ */
/* middlewares                                                         */
/* ------------------------------------------------------------------ */

/* Validación común de verify/refresh: token GOST + csrf + salt del DAO.
 * Publica el usuario en req->__user en éxito. 1 si autoriza, 0 si ya
 * respondió 401/500. */
static int authorize_request(authorization_t* auth, cws_request_t* req,
                             cws_response_t* res,
                             jwt_gost_payload_t* payload) {
    /* userDAO(hash(usu_correo)) — la BD guarda el correo hasheado (igual
     * que signin). El token lleva el correo en claro. */
    char* hashed = jwt_core_hash_hex(payload->user.usu_correo);
    sql_result_t* dao = hashed ? authorization_user_dao(auth, hashed) : NULL;
    free(hashed);
    if (!dao || dao->nrows == 0) {
        send_json_error(res, 500, "Error al iniciar sesión");
        sql_result_free(dao);
        return 0;
    }

    /* salt del DAO vs salt del token (trim + comparación timing-safe). */
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
    if (!salt_ok) {
        free(dao_salt);
        send_json_error(res, 401, "Error al iniciar sesión");
        return 0;
    }
    free(dao_salt);

    auth_ctx_t* ctx = (auth_ctx_t*)calloc(1, sizeof(*ctx));
    if (!ctx) {
        send_json_error(res, 500, "Error al iniciar sesión");
        return 0;
    }
    ctx->authorized = 1;
    ctx->payload = payload;
    req->__user = ctx;
    return 1;
}

static void free_ctx(cws_request_t* req) {
    auth_ctx_t* ctx = (auth_ctx_t*)req->__user;
    if (ctx) {
        jwt_payload_free(ctx->payload);
        free(ctx);
        req->__user = NULL;
    }
}

void cws_mw_authorization_verify(cws_request_t* req, cws_response_t* res,
                                 cws_next_fn next) {
    if (!g_auth) {
        send_json_error(res, 500, "Servicio no inicializado");
        return;
    }

    char* token = cookie_value(req, "access_token");
    char* csrf_token = header_dup(req, "x-csrf-token");
    if (!token || !csrf_token) {
        send_json_error(res, 401, "No credentials are present.");
        free(token);
        free(csrf_token);
        return;
    }

    jwt_gost_payload_t* payload = jwt_gost_verify(g_auth->jwt, token);
    bool csrf_valid = jwt_verify_csrf_token(g_auth->jwt, csrf_token);
    free(token);
    free(csrf_token);

    if (!payload || !csrf_valid) {
        jwt_payload_free(payload);
        send_json_error(res, 401, "Authentication rejected.");
        return;
    }

    /* Verificación de IP (canal web): el token fue emitido para una IP y solo
     * se acepta desde esa misma IP. Mobile lo omite. */
    char* ip = client_ip(res);
    int ip_ok = ip && payload->user.ip &&
                strcmp(ip, payload->user.ip) == 0;
    free(ip);
    if (!ip_ok) {
        jwt_payload_free(payload);
        send_json_error(res, 401, "Authentication rejected.");
        return;
    }

    if (!authorize_request(g_auth, req, res, payload)) {
        jwt_payload_free(payload);
        free_ctx(req);
        return;
    }

    /* Pipeline síncrono: al volver de next() la petición terminó. */
    next(req, res);
    free_ctx(req);
}

void cws_mw_authorization_refresh(cws_request_t* req, cws_response_t* res,
                                  cws_next_fn next) {
    if (!g_auth) {
        send_json_error(res, 500, "Servicio no inicializado");
        return;
    }

    char* token = cookie_value(req, "refresh_token");
    char* refresh_csrf = header_dup(req, "x-csrf-token");
    if (!token || !refresh_csrf) {
        send_json_error(res, 401, "No credentials are present.");
        free(token);
        free(refresh_csrf);
        return;
    }

    jwt_gost_payload_t* payload = jwt_verify_refresh_token(g_auth->jwt, token);
    bool csrf_valid = jwt_verify_csrf_token(g_auth->jwt, refresh_csrf);
    free(token);
    free(refresh_csrf);

    if (!payload || !csrf_valid) {
        jwt_payload_free(payload);
        send_json_error(res, 401, "Authentication rejected.");
        return;
    }

    /* IP del canal web: el refresh también va ligado a la IP original. */
    char* ip = client_ip(res);
    int ip_ok = ip && payload->user.ip &&
                strcmp(ip, payload->user.ip) == 0;
    free(ip);
    if (!ip_ok) {
        jwt_payload_free(payload);
        send_json_error(res, 401, "Authentication rejected.");
        return;
    }

    if (!authorize_request(g_auth, req, res, payload)) {
        jwt_payload_free(payload);
        free_ctx(req);
        return;
    }

    /* Re-emisión de access/csrf/refresh_csrf (cookies). El access se firma
     * con user->ip (el mismo del token original, ya validado). */
    jwt_user_t user = payload->user;
    char* access = jwt_write_gost_token(g_auth->jwt, &user);
    char* csrf = jwt_write_csrf_token(g_auth->jwt);
    char* refresh_csrf_new = jwt_write_refresh_csrf_token(g_auth->jwt);

    if (access)
        set_cookie(res, "access_token", access, "/api/v1",
                   g_auth->access_minutes * 60, 1, g_auth->is_prod);
    if (csrf)
        set_cookie(res, "csrf_token", csrf, "/",
                   g_auth->access_minutes * 60, 0, g_auth->is_prod);
    if (refresh_csrf_new)
        set_cookie(res, "refresh_csrf_token", refresh_csrf_new, "/",
                   g_auth->refresh_days * 86400, 0, g_auth->is_prod);

    free(access);
    free(csrf);
    free(refresh_csrf_new);

    /* Pipeline síncrono: al retornar next() la petición terminó. */
    next(req, res);
    free_ctx(req);
}

/* Endpoint POST /api/v1/users/refresh_token: misma validación que el
 * middleware, re-emite cookies y responde JSON (sin continuar cadena). */
void authorization_refresh(cws_request_t* req, cws_response_t* res) {
    if (!g_auth) {
        send_json_error(res, 500, "Servicio no inicializado");
        return;
    }

    char* token = cookie_value(req, "refresh_token");
    char* refresh_csrf = header_dup(req, "x-csrf-token");
    if (!token || !refresh_csrf) {
        send_json_error(res, 401, "No credentials are present.");
        free(token);
        free(refresh_csrf);
        return;
    }

    jwt_gost_payload_t* payload = jwt_verify_refresh_token(g_auth->jwt, token);
    bool csrf_valid = jwt_verify_csrf_token(g_auth->jwt, refresh_csrf);
    free(token);
    free(refresh_csrf);

    if (!payload || !csrf_valid) {
        jwt_payload_free(payload);
        send_json_error(res, 401, "Authentication rejected.");
        return;
    }

    char* ip = client_ip(res);
    int ip_ok = ip && payload->user.ip && strcmp(ip, payload->user.ip) == 0;
    free(ip);
    if (!ip_ok) {
        jwt_payload_free(payload);
        send_json_error(res, 401, "Authentication rejected.");
        return;
    }

    if (!authorize_request(g_auth, req, res, payload)) {
        jwt_payload_free(payload);
        free_ctx(req);
        return;
    }

    jwt_user_t user = payload->user;
    char* access = jwt_write_gost_token(g_auth->jwt, &user);
    char* csrf = jwt_write_csrf_token(g_auth->jwt);
    char* refresh_csrf_new = jwt_write_refresh_csrf_token(g_auth->jwt);

    if (access)
        set_cookie(res, "access_token", access, "/api/v1",
                   g_auth->access_minutes * 60, 1, g_auth->is_prod);
    if (csrf)
        set_cookie(res, "csrf_token", csrf, "/",
                   g_auth->access_minutes * 60, 0, g_auth->is_prod);
    if (refresh_csrf_new)
        set_cookie(res, "refresh_csrf_token", refresh_csrf_new, "/",
                   g_auth->refresh_days * 86400, 0, g_auth->is_prod);

    free(access);
    free(csrf);
    free(refresh_csrf_new);

    const char* body = "{\"success\":true,\"error\":0,"
                       "\"msg\":\"Sesion renovada\"}";
    cws_response_status(res, 200);
    cws_response_body(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
    free_ctx(req);
}

/* ------------------------------------------------------------------ */
/* signin                                                              */
/* ------------------------------------------------------------------ */

/* Cuerpo JSON NUL-terminado malloc (req->body no lo está). */
static char* body_dup(const cws_request_t* req) {
    if (!req || !req->body || req->body_len == 0) return NULL;
    size_t len = req->body_len > 8192 ? 8192 : req->body_len;
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, req->body, len);
    out[len] = '\0';
    return out;
}

/* Cookie Set-Cookie con atributos de la referencia. `secure` según
 * NODE_ENV. `path` y `max_age` dependen del token. */
static void set_cookie(cws_response_t* res, const char* name,
                       const char* value, const char* path,
                       int max_age_sec, int http_only, int secure) {
    /* Dominio opcional (para compartir cookies entre subdominios, p. ej.
     * el front en sodastream.fun y la API en cws.sodastream.fun). */
    char dom[160];
    dom[0] = '\0';
    const char* d = cfg_getenv("COOKIE_DOMAIN");
    if (d && *d) snprintf(dom, sizeof(dom), "; Domain=%s", d);

    char cookie[1600];
    int n = snprintf(cookie, sizeof(cookie),
                     "%s=%s%s%s%s; SameSite=Lax; Path=%s; Max-Age=%d",
                     name, value ? value : "",
                     http_only ? "; HttpOnly" : "",
                     secure ? "; Secure" : "",
                     dom, path, max_age_sec);
    if (n > 0 && (size_t)n < sizeof(cookie))
        cws_response_header(res, "Set-Cookie", "%s", cookie);
}

int authorization_signin(cws_request_t* req, cws_response_t* res,
                         jwt_user_t* out) {
    if (!g_auth || !req || !out) return CWS_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    char* body = body_dup(req);
    if (!body) return CWS_ERR_INVALID;

    char* email = NULL;
    char* password = NULL;
    if (jwt_json_get_string(body, "usu_correo", &email) != CWS_OK ||
        jwt_json_get_string(body, "usu_contrasena", &password) != CWS_OK) {
        free(body);
        free(email);
        free(password);
        return CWS_ERR_INVALID;
    }

    /* usu_correo.trim().toLowerCase() */
    trim_inplace(email);
    for (char* p = email; *p; p++) *p = (char)tolower((unsigned char)*p);

    /* hashed_email = hash(email): el DAO busca por el hash. */
    char* hashed = jwt_core_hash_hex(email);
    if (!hashed) {
        free(body);
        free(email);
        free(password);
        return CWS_ERR_GENERIC;
    }

    sql_result_t* dao = authorization_user_dao(g_auth, hashed);
    free(hashed);
    if (!dao || dao->nrows == 0) {
        sql_result_free(dao);
        free(body);
        free(email);
        free(password);
        return CWS_ERR_NOTFOUND;
    }

    char* stored_hash = dao_str(dao, "usu_contrasena");
    int64_t usu_id = dao_int(dao, "usu_id", 0);
    char* usu_nombre = dao_str(dao, "usu_nombre");
    char* usu_salt = dao_str(dao, "usu_salt");
    sql_result_free(dao);

    password_result_t pr = password_verify(password, stored_hash ? stored_hash : "");
    if (pr == PASSWORD_OK_LEGACY)
        cws_log_warn("signin: hash legacy de usu_id=%lld (conviene rehashear)",
                     (long long)usu_id);
    if (pr != PASSWORD_OK && pr != PASSWORD_OK_LEGACY) {
        free(stored_hash);
        free(usu_nombre);
        free(usu_salt);
        free(body);
        free(email);
        free(password);
        return CWS_ERR_NOTFOUND;
    }

    /* Tokens. */
    char* ip = client_ip(res);
    jwt_user_t dao_user = {
        .usu_id = usu_id,
        .usu_nombre = usu_nombre,
        .usu_correo = email,
        .ip = ip,
        .usu_salt = usu_salt,
    };
    char* access_token = jwt_write_gost_token(g_auth->jwt, &dao_user);
    char* refresh_token = jwt_write_refresh_token(g_auth->jwt, &dao_user);
    char* csrf_token = jwt_write_csrf_token(g_auth->jwt);
    char* refresh_csrf_token = jwt_write_refresh_csrf_token(g_auth->jwt);
    free(ip);

    int is_prod = g_auth->is_prod;

    set_cookie(res, "access_token", access_token, "/api/v1",
               g_auth->access_minutes * 60, 1, is_prod);
    set_cookie(res, "csrf_token", csrf_token, "/",
               g_auth->access_minutes * 60, 0, is_prod);
    set_cookie(res, "refresh_token", refresh_token,
               "/api/v1/users/refresh_token",
               g_auth->refresh_days * 86400, 1, is_prod);
    set_cookie(res, "refresh_csrf_token", refresh_csrf_token, "/",
               g_auth->refresh_days * 86400, 0, is_prod);

    /* Salida. */
    out->usu_id = usu_id;
    out->usu_nombre = usu_nombre;
    out->usu_correo = strdup(email);
    out->usu_salt = strdup(usu_salt ? usu_salt : "");

    free(access_token);
    free(refresh_token);
    free(csrf_token);
    free(refresh_csrf_token);
    free(stored_hash);
    free(body);
    free(email);
    free(password);
    return CWS_OK;
}

void authorization_user_free(jwt_user_t* user) {
    if (!user) return;
    free(user->usu_nombre);
    free(user->usu_correo);
    free(user->ip);
    free(user->usu_salt);
    memset(user, 0, sizeof(*user));
}
