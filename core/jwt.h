#ifndef JWT_H
#define JWT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "cws/cws.h"
#include "configuration.h"
#include "jwt_core.h"   /* re-exporta jwt_user_t, jwt_core_payload_t,
                           jwt_safe_compare, jwt_json_get_*, jwt_core_hash_hex,
                           jwt_core_verify_password, jwt_core_generate_password,
                           jwt_core_write_*, jwt_core_decode,
                           jwt_core_verify_*, jwt_core_payload_free. */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * jwt — canal web: emite y verifica tokens GOST para autenticación por
 * cookie (AuthorizationService). Sobre core/jwt_core (capa compartida con
 * jwt_mobile). Configuración por core/configuration:
 *   - session_type = cfg->web_session_type  (default 1; configurable vía
 *     .env WEB_SESSION_TYPE)
 *   - expiration    = cfg->access_token_expiration_minutes * 60000
 *   - refresh_exp   = cfg->refresh_token_expiration_days * 86400000
 *
 * Diferencias con jwt_mobile:
 *   - session_type distinto.
 *   - sign_tail del access = user->ip (y el middleware verifica IP).
 *   - refresh usa el sufijo "refresh".
 *   - expone CSRF (no usado en móvil).
 */

/* Alias por compatibilidad con código previo (authorization.c). */
typedef jwt_core_payload_t jwt_gost_payload_t;

/** Tamaño hash hex de un token (64 chars + NUL). */
#define JWT_HASH_HEX 64

typedef struct jwt jwt_t;

/**
 * \brief Construye el servicio web desde la configuración.
 *        NULL si falla la creación del núcleo (X_VECTOR inválido).
 */
jwt_t* jwt_new(const app_config_t* cfg);
void jwt_free(jwt_t* jwt);

/* ------------------------------------------------------------------ */
/* tokens                                                              */
/* ------------------------------------------------------------------ */

/**
 * \brief Access token web: payload {id, init, exp, session_type, user, sign}
 *        con sign = hash(secret + id + user->ip). El middleware (authorization)
 *        valida además que user->ip coincida con la IP de la petición.
 */
char* jwt_write_gost_token(const jwt_t* jwt, const jwt_user_t* user);

/**
 * \brief Verifica access token. session_type debe ser el del canal; sign
 *        se calcula con sign_tail = payload->user.ip.
 * \return payload malloc (liberar con jwt_payload_free) o NULL.
 */
jwt_gost_payload_t* jwt_gost_verify(const jwt_t* jwt, const char* token);

/** Refresh token web: sign = hash(secret + id + "refresh"). */
char* jwt_write_refresh_token(const jwt_t* jwt, const jwt_user_t* user);
jwt_gost_payload_t* jwt_verify_refresh_token(const jwt_t* jwt, const char* token);

/** CSRF de sesión (2 h) y de refresh (REFRESH_TOKEN_EXPIRATION_DAYS). */
char* jwt_write_csrf_token(const jwt_t* jwt);
char* jwt_write_refresh_csrf_token(const jwt_t* jwt);
bool   jwt_verify_csrf_token(const jwt_t* jwt, const char* token);

/* ------------------------------------------------------------------ */
/* contraseñas (compat — el canal móvil las delega a core también)        */
/* ------------------------------------------------------------------ */

/** Reversible (CFB) de trim(password) en base64. */
char* jwt_generate_hash(const jwt_t* jwt, const char* password);

/** Verificación contra hash Streebog-256 hex (timing-safe). */
bool jwt_gost_hash_verify(const jwt_t* jwt, const char* password,
                          const char* hash);

/* ------------------------------------------------------------------ */
/* lifecycle helpers                                                   */
/* ------------------------------------------------------------------ */

/** Libera el payload devuelto por jwt_gost_verify / jwt_verify_refresh_token. */
void jwt_payload_free(jwt_gost_payload_t* payload);

#ifdef __cplusplus
}
#endif

#endif /* JWT_H */
