#ifndef PASSWORD_H
#define PASSWORD_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * password — hash y verificación de contraseñas con KDF GOST.
 *
 * Fix #2 del análisis de seguridad: en lugar de Streebog-256(password)
 * (hash rápido y sin salt), se usa PBKDF2 con HMAC-Streebog (feature
 * gost/kdf) + salt aleatorio por contraseña + pepper opcional del servidor.
 *
 * Formato almacenado (auto-descriptivo, permite subir parámetros luego):
 *
 *   $gost-pbkdf2$512$<iteraciones>$<salt_hex>$<dk_hex>
 *
 * - salt: 16 bytes aleatorios (gost_rng_system), no secreto.
 * - dk:   32 bytes (PBKDF2 con PRF HMAC-Streebog-512, truncado).
 * - pepper: secreto del servidor (env PASSWORD_PEPPER, fuera de la BD); se
 *   prefija a la contraseña antes del KDF. Si está vacío, no aplica.
 * - iteraciones: env PASSWORD_KDF_ITERATIONS (default PASSWORD_DEFAULT_ITER).
 *
 * Compatibilidad: password_verify() sigue aceptando el formato legacy
 * (Streebog-256 hex) y avisa con PASSWORD_OK_LEGACY para poder rehashear.
 */

/** Iteraciones por defecto si no se configura PASSWORD_KDF_ITERATIONS. */
#define PASSWORD_DEFAULT_ITER 100000u

/** Resultado de la verificación. */
typedef enum password_result {
    PASSWORD_OK = 0,      /* verificado con el KDF actual */
    PASSWORD_OK_LEGACY,   /* verificado con el formato viejo; conviene rehash */
    PASSWORD_FAIL,        /* credenciales inválidas */
    PASSWORD_ERROR,       /* error de entrada/memoria */
} password_result_t;

/**
 * \brief Genera el hash almacenable de una contraseña.
 * \return string malloc (liberar con free) o NULL si falla.
 */
char* password_hash(const char* password);

/**
 * \brief Verifica una contraseña contra un hash almacenado (formato actual
 *        o legacy).
 * \return PASSWORD_OK / PASSWORD_OK_LEGACY / PASSWORD_FAIL / PASSWORD_ERROR.
 */
password_result_t password_verify(const char* password, const char* stored);

/**
 * \brief true si `stored` no usa el formato/iteraciones actuales
 *        (conviene rehashear tras un login exitoso).
 */
bool password_needs_rehash(const char* stored);

#ifdef __cplusplus
}
#endif

#endif /* PASSWORD_H */
