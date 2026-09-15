/*
 * jwt.c — canal web: capa delgada sobre core/jwt_core. Configura
 * session_type (de cfg->web_session_type), expiraciones, y el sign_tail
 * del access token (user->ip, validado por el middleware).
 */

#define _POSIX_C_SOURCE 200809L

#include "jwt.h"

#include <gost/gost.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define JWT_WEB_REFRESH_SUFFIX "refresh"
#define JWT_WEB_CSRF_SESSION_MS (2LL * 60 * 60 * 1000)
#define JWT_WEB_CSRF_REFRESH_SUFFIX "refresh_csrf"

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

struct jwt {
    jwt_core_t*    core;
    int64_t        session_type;
    int64_t        access_ms;
    int64_t        refresh_ms;
};

jwt_t* jwt_new(const app_config_t* cfg) {
    if (!cfg) return NULL;
    jwt_t* j = (jwt_t*)calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->core = jwt_core_new(cfg->secret_key, cfg->x_vector);
    if (!j->core) { free(j); return NULL; }
    j->session_type = (int64_t)cfg->web_session_type;
    j->access_ms    = (int64_t)cfg->access_token_expiration_minutes * 60000LL;
    j->refresh_ms   = (int64_t)cfg->refresh_token_expiration_days * 86400000LL;
    return j;
}

void jwt_free(jwt_t* jwt) {
    if (!jwt) return;
    jwt_core_free(jwt->core);
    free(jwt);
}

/* El middleware (authorization.c) compara además user->ip con la IP de la
 * petición; aquí no tenemos req, así que el sign usa user->ip y el middleware
 * lo valida al recibir el payload. */
char* jwt_write_gost_token(const jwt_t* jwt, const jwt_user_t* user) {
    if (!jwt || !user) return NULL;
    return jwt_core_write_access(jwt->core, user, jwt->session_type,
                                 now_ms() + jwt->access_ms, user->ip);
}

jwt_gost_payload_t* jwt_gost_verify(const jwt_t* jwt, const char* token) {
    if (!jwt || !token) return NULL;
    jwt_core_payload_t* p = jwt_core_decode(jwt->core, token);
    if (!p) return NULL;
    if (p->kind == JWT_CORE_KIND_CSRF ||
        p->session_type != jwt->session_type ||
        !jwt_core_verify_sign(jwt->core, p, p->user.ip)) {
        jwt_core_payload_free(p);
        return NULL;
    }
    return p;
}

char* jwt_write_refresh_token(const jwt_t* jwt, const jwt_user_t* user) {
    if (!jwt || !user) return NULL;
    return jwt_core_write_refresh(jwt->core, user, jwt->session_type,
                                 now_ms() + jwt->refresh_ms,
                                 JWT_WEB_REFRESH_SUFFIX);
}

jwt_gost_payload_t* jwt_verify_refresh_token(const jwt_t* jwt,
                                             const char* token) {
    if (!jwt || !token) return NULL;
    jwt_core_payload_t* p = jwt_core_decode(jwt->core, token);
    if (!p) return NULL;
    if (p->kind != JWT_CORE_KIND_REFRESH ||
        p->session_type != jwt->session_type ||
        !jwt_core_verify_sign(jwt->core, p, JWT_WEB_REFRESH_SUFFIX)) {
        jwt_core_payload_free(p);
        return NULL;
    }
    return p;
}

char* jwt_write_csrf_token(const jwt_t* jwt) {
    if (!jwt) return NULL;
    return jwt_core_write_csrf(jwt->core,
                              now_ms() + JWT_WEB_CSRF_SESSION_MS,
                              NULL, NULL);
}

char* jwt_write_refresh_csrf_token(const jwt_t* jwt) {
    if (!jwt) return NULL;
    return jwt_core_write_csrf(jwt->core,
                              now_ms() + jwt->refresh_ms,
                              "refresh_csrf", JWT_WEB_CSRF_REFRESH_SUFFIX);
}

bool jwt_verify_csrf_token(const jwt_t* jwt, const char* token) {
    if (!jwt || !token) return false;
    /* Detectar sufijo por longitud/forma: probamos ambos. */
    if (jwt_core_verify_csrf(jwt->core, token, NULL)) return true;
    return jwt_core_verify_csrf(jwt->core, token, JWT_WEB_CSRF_REFRESH_SUFFIX);
}

char* jwt_generate_hash(const jwt_t* jwt, const char* password) {
    if (!jwt) return NULL;
    return jwt_core_generate_password(jwt->core, password);
}

bool jwt_gost_hash_verify(const jwt_t* jwt, const char* password,
                          const char* hash) {
    if (!jwt) return false;
    return jwt_core_verify_password(password, hash);
}

void jwt_payload_free(jwt_gost_payload_t* payload) {
    jwt_core_payload_free(payload);
}
