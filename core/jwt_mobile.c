/*
 * jwt_mobile.c — canal móvil: capa delgada sobre core/jwt_core.
 * No valida IP (Android); usa sign_tail fijo "mobile" / "mobile_refresh".
 */

#define _POSIX_C_SOURCE 200809L

#include "jwt_mobile.h"

#include <gost/gost.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define JWT_MOBILE_ACCESS_SUFFIX   "mobile"
#define JWT_MOBILE_REFRESH_SUFFIX  "mobile_refresh"

struct jwt_mobile {
    jwt_core_t* core;
    int64_t     session_type;
    int64_t     access_ms;
    int64_t     refresh_ms;
};

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

jwt_mobile_t* jwt_mobile_new(const app_config_t* cfg) {
    if (!cfg) return NULL;
    jwt_mobile_t* m = (jwt_mobile_t*)calloc(1, sizeof(*m));
    if (!m) return NULL;
    m->core = jwt_core_new(cfg->secret_key, cfg->x_vector);
    if (!m->core) { free(m); return NULL; }
    m->session_type = (int64_t)cfg->mobile_session_type;
    m->access_ms    = (int64_t)cfg->access_token_expiration_minutes * 60000LL;
    m->refresh_ms   = (int64_t)cfg->refresh_token_expiration_days * 86400000LL;
    return m;
}

void jwt_mobile_free(jwt_mobile_t* m) {
    if (!m) return;
    jwt_core_free(m->core);
    free(m);
}

char* jwt_mobile_write_access_token(const jwt_mobile_t* m,
                                   const jwt_user_t* user) {
    if (!m || !user) return NULL;
    /* El access móvil tiene la misma duración que el web (access_ms); el
     * refresh móvil vive refresh_ms (configurable). */
    return jwt_core_write_access(m->core, user, m->session_type,
                                 now_ms() + m->access_ms,
                                 JWT_MOBILE_ACCESS_SUFFIX);
}

jwt_mobile_payload_t jwt_mobile_verify_access_token(const jwt_mobile_t* m,
                                                   const char* token) {
    if (!m || !token) return NULL;
    jwt_core_payload_t* p = jwt_core_decode(m->core, token);
    if (!p) return NULL;
    if (p->kind == JWT_CORE_KIND_CSRF ||
        p->session_type != m->session_type ||
        !jwt_core_verify_sign(m->core, p, JWT_MOBILE_ACCESS_SUFFIX)) {
        jwt_core_payload_free(p);
        return NULL;
    }
    return p;
}

char* jwt_mobile_write_refresh_token(const jwt_mobile_t* m,
                                    const jwt_user_t* user) {
    if (!m || !user) return NULL;
    return jwt_core_write_refresh(m->core, user, m->session_type,
                                 now_ms() + m->refresh_ms,
                                 JWT_MOBILE_REFRESH_SUFFIX);
}

jwt_mobile_payload_t jwt_mobile_verify_refresh_token(const jwt_mobile_t* m,
                                                   const char* token) {
    if (!m || !token) return NULL;
    jwt_core_payload_t* p = jwt_core_decode(m->core, token);
    if (!p) return NULL;
    if (p->kind != JWT_CORE_KIND_REFRESH ||
        p->session_type != m->session_type ||
        !jwt_core_verify_sign(m->core, p, JWT_MOBILE_REFRESH_SUFFIX)) {
        jwt_core_payload_free(p);
        return NULL;
    }
    return p;
}

void jwt_mobile_payload_free(jwt_mobile_payload_t payload) {
    jwt_core_payload_free(payload);
}
