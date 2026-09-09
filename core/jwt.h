#ifndef JWT_H
#define JWT_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "cws/cws.h"
#include "configuration.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * JsonWebToken — emisión y verificación de tokens de sesión.
 *
 * Adaptación en C de reference/jwt.js (class JsonWebToken), usando
 * directamente el port C de GOST (submódulo third_party/c_gost_encryption)
 * en lugar del intermediario encrypt.js:
 *
 *   jwt.js                                  jwt.c
 *   ─────────────────────────────────────   ──────────────────────────────
 *   encrypt.reversible_encrypt(payload)  →  jwt_write_gost_token() etc.
 *     (Строку_в_байты + Шифрование.        (UTF-16LE + gost_cipher_cfb
 *      Гаммование_с_обратной_связью         + base64)
 *      + base64)
 *   encrypt.decrypt(token)               →  jwt_gost_verify() etc.
 *   encrypt.hash(data)                   →  gost_hash(..., GOST_HASH_256)
 *   crypto.timingSafeEqual               →  jwt_safe_compare()
 *   crypto.randomBytes(32).hex           →  gost_rng_system
 *
 * Convenciones (iguales que la referencia):
 *   - Clave del cifrado: X_VECTOR (8 palabras hex), tabla TC26-Z.
 *   - IV del CFB: primeros 8 bytes de SECRET_KEY en LE; {0,0} si vacía.
 *   - Payload serializado como JSON, codificado UTF-16LE antes de cifrar.
 *   - session_type de sesión web = JWT_SESSION_TYPE (1).
 *   - Los timestamps son milisegundos desde epoch.
 */

/** Tipo de sesión web (WEB_SESSION_TYPE en la referencia). */
#define JWT_WEB_SESSION_TYPE 1

/** Capacidad hex de 32 bytes aleatorios (64 chars + NUL). */
#define JWT_RANDOM_HEX 64

typedef struct jwt jwt_t;

/** Usuario codificado dentro del payload del token. */
typedef struct jwt_user {
    int64_t  usu_id;        /* id del usuario.                     */
    char*    usu_nombre;    /* propiedad del payload; NULL si no.  */
    char*    usu_correo;    /* propiedad del payload.              */
    char*    ip;            /* IP de la petición de origen.        */
    char*    usu_salt;      /* salt del DAO al emitir el token.    */
} jwt_user_t;

/** Payload decodificado de un token GOST (gost_verify / refresh). */
typedef struct jwt_gost_payload {
    int64_t     id;            /* usu_id raíz del payload.          */
    int64_t     init;          /* emisión (ms epoch).               */
    int64_t     exp;           /* expiración (ms epoch).            */
    int64_t     session_type;  /* WEB_SESSION_TYPE.                 */
    jwt_user_t  user;          /* usuario embebido.                 */
    char*       sign;          /* firma hex embebida.               */
} jwt_gost_payload_t;

/* ------------------------------------------------------------------ */
/* ciclo de vida                                                       */
/* ------------------------------------------------------------------ */

/**
 * \brief Construye el servicio de tokens desde la configuración central.
 *
 * Parsea X_VECTOR (clave de cifrado) y SECRET_KEY (IV + firma de payload).
 * NULL si falta configuración o X_VECTOR no tiene 8 palabras válidas.
 */
jwt_t* jwt_new(const app_config_t* cfg);
void   jwt_free(jwt_t* jwt);

/* ------------------------------------------------------------------ */
/* utilidades                                                          */
/* ------------------------------------------------------------------ */

/**
 * \brief Comparación en tiempo constante (crypto.timingSafeEqual).
 * \return true si a y b son iguales (longitud y bytes).
 */
bool jwt_safe_compare(const char* a, const char* b);

/**
 * \brief Extrae un miembro string de un objeto JSON plano.
 *
 * Utilidad del parser JSON interno (válido para los payloads de este
 * módulo y cuerpos de petición simples).
 * \return CWS_OK y `*out` malloc (liberar con free), o CWS_ERR_INVALID.
 */
int jwt_json_get_string(const char* json, const char* name, char** out);

/** \brief Extrae un miembro numérico. CWS_OK o CWS_ERR_INVALID. */
int jwt_json_get_int(const char* json, const char* name, int64_t* out);

/* ------------------------------------------------------------------ */
/* tokens GOST (access / refresh)                                      */
/* ------------------------------------------------------------------ */

/**
 * \brief Emite el access token GOST (equivalente a write_gost_token).
 *
 * Payload: {id, init, exp, session_type, user{usu_id, usu_nombre,
 * usu_correo, ip, usu_salt}, sign} — sign = hash(secret + usu_id + ip) —
 * cifrado reversible y devuelto en base64.
 *
 * \param[in] jwt   servicio.
 * \param[in] user  datos del DAO autenticado; user.ip es la IP de la
 *                  petición (req.ip) y forma parte del payload y del sign.
 * \return token base64 malloc (liberar con free), o NULL.
 */
char* jwt_write_gost_token(const jwt_t* jwt, const jwt_user_t* user);

/**
 * \brief Verifica un access token GOST (equivalente a gost_verify).
 *
 * Descifra, valida la firma embebida (timing-safe), la expiración y el
 * session_type.
 *
 * \return payload malloc (liberar con jwt_payload_free), NULL si es
 *         inválido o expirado.
 */
jwt_gost_payload_t* jwt_gost_verify(const jwt_t* jwt, const char* token);

/**
 * \brief Emite el refresh token (equivalente a write_refresh_token).
 *
 * Payload {id, exp, type:'refresh', session_type, user{...},
 * sign = hash(secret + usu_id + 'refresh')}, cifrado reversible.
 */
char* jwt_write_refresh_token(const jwt_t* jwt, const jwt_user_t* user);

/**
 * \brief Verifica un refresh token (equivalente a verify_refresh_token).
 * \return payload malloc, NULL si es inválido o expirado.
 */
jwt_gost_payload_t* jwt_verify_refresh_token(const jwt_t* jwt,
                                             const char* token);

/** Libera un payload devuelto por jwt_gost_verify/jwt_verify_refresh_token. */
void jwt_payload_free(jwt_gost_payload_t* payload);

/* ------------------------------------------------------------------ */
/* CSRF tokens                                                         */
/* ------------------------------------------------------------------ */

/**
 * \brief CSRF token de sesión (equivalente a write_csrf_token): 2 h de vida,
 *        rand de 32 bytes hex y sign = hash(secret + rand + exp).
 * \return token base64 malloc, o NULL.
 */
char* jwt_write_csrf_token(const jwt_t* jwt);

/**
 * \brief CSRF token de refresh (write_refresh_csrf_token): vida de
 *        REFRESH_TOKEN_EXPIRATION_DAYS, type:'refresh_csrf' y
 *        sign = hash(secret + rand + exp + 'refresh_csrf').
 */
char* jwt_write_refresh_csrf_token(const jwt_t* jwt);

/**
 * \brief Verifica un CSRF token de sesión o de refresh
 *        (equivalente a verify_csrf_token).
 * \return true si la firma coincide y no está expirado.
 */
bool jwt_verify_csrf_token(const jwt_t* jwt, const char* token);

/* ------------------------------------------------------------------ */
/* contraseñas                                                         */
/* ------------------------------------------------------------------ */

/**
 * \brief Hash Streebog-256 en hex: equivalente a gost.Хэшевание.Вычислить
 *        (y a EncryptService.hash del intermediario).
 * \return buffer malloc con 64 chars hex + NUL (liberar con free), o NULL.
 */
char* jwt_hash_hex(const char* data);

/**
 * \brief Hash de contraseña para almacenar (generate_hash):
 *        token reversible de password.trim().
 * \return token base64 malloc, o NULL.
 */
char* jwt_generate_hash(const jwt_t* jwt, const char* password);

/**
 * \brief Verifica contraseña contra el hash almacenado
 *        (gost_hash_verify): compara timing-safe el Streebog-256 hex del
 *        password contra `hash`.
 * \return true si coinciden.
 */
bool jwt_gost_hash_verify(const jwt_t* jwt, const char* password,
                          const char* hash);

#ifdef __cplusplus
}
#endif

#endif /* JWT_H */
