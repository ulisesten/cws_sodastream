#ifndef JWT_MOBILE_H
#define JWT_MOBILE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "cws/cws.h"
#include "configuration.h"
#include "jwt_core.h"   /* re-exporta jwt_user_t y todas las primitivas. */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * jwt_mobile — canal móvil: tokens GOST para autenticación de apps Android.
 * Sobre core/jwt_core (compartido con jwt web). Diferencias:
 *   - session_type = cfg->mobile_session_type (default 2; configurable vía
 *     .env MOBILE_SESSION_TYPE).
 *   - sign_tail del access NO contiene la IP (mobile no validará la IP).
 *   - refresh usa sufijo "mobile_refresh".
 *   - Sin CSRF (no aplica a móvil).
 */

typedef jwt_core_payload_t* jwt_mobile_payload_t;

typedef struct jwt_mobile jwt_mobile_t;

/* Construye el servicio móvil desde la configuración central. */
jwt_mobile_t* jwt_mobile_new(const app_config_t* cfg);
void jwt_mobile_free(jwt_mobile_t* m);

/**
 * \brief Access token móvil: sign = hash(secret + id + "mobile"). El
 *        middleware NO valida la IP.
 */
char* jwt_mobile_write_access_token(const jwt_mobile_t* m,
                                   const jwt_user_t* user);

/**
 * \brief Verifica access token. session_type debe ser el móvil; sign se
 *        valida con el sufijo "mobile".
 */
jwt_mobile_payload_t jwt_mobile_verify_access_token(const jwt_mobile_t* m,
                                                   const char* token);

/** Refresh token móvil: sign = hash(secret + id + "mobile_refresh"). */
char* jwt_mobile_write_refresh_token(const jwt_mobile_t* m,
                                    const jwt_user_t* user);
jwt_mobile_payload_t jwt_mobile_verify_refresh_token(const jwt_mobile_t* m,
                                                   const char* token);

void jwt_mobile_payload_free(jwt_mobile_payload_t payload);

#ifdef __cplusplus
}
#endif

#endif /* JWT_MOBILE_H */
