#ifndef JWT_CORE_H
#define JWT_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <gost/gost.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * jwt_core — primitivas compartidas entre jwt (web) y jwt_mobile.
 *
 * El núcleo posee el contexto criptográfico (clave + IV del CFB), las
 * funciones de token de bajo nivel (cifrar/descifrar/serializar/firmar
 * payloads GOST con la convención de la referencia: UTF-16LE + Streebog) y
 * los helpers de JSON, base64 y comparación en tiempo constante. Los
 * módulos web y móvil lo configuran con su session_type y sus sufijos de
 * firma (configurables desde core/configuration).
 */

/** 32 bytes aleatorios -> 64 hex + NUL (estilo nanoid). */
#define JWT_CORE_RANDOM_HEX 64

typedef struct jwt_core jwt_core_t;

/** Usuario codificado dentro del payload del token. */
typedef struct jwt_user {
    int64_t  usu_id;
    char*    usu_nombre;
    char*    usu_correo;
    char*    ip;
    char*    usu_salt;
} jwt_user_t;

typedef enum {
    JWT_CORE_KIND_ACCESS,
    JWT_CORE_KIND_REFRESH,
    JWT_CORE_KIND_CSRF
} jwt_core_kind_t;

typedef struct jwt_core_payload {
    jwt_core_kind_t kind;
    int64_t  id;            /* ACCESS, REFRESH */
    int64_t  init;          /* ACCESS */
    int64_t  exp;           /* todos */
    int64_t  session_type;  /* ACCESS, REFRESH */
    char*    type;          /* REFRESH ("refresh"), CSRF ("refresh_csrf") */
    char*    rand;          /* CSRF */
    jwt_user_t user;        /* ACCESS, REFRESH */
    char*    sign;          /* todos */
} jwt_core_payload_t;

/* ------------------------------------------------------------------ */
/* ciclo de vida                                                       */
/* ------------------------------------------------------------------ */

/**
 * \brief Crea el núcleo a partir de SECRET_KEY + X_VECTOR de la
 *        configuración (centralizada en core/configuration).
 *        NULL si faltan/son inválidos.
 */
jwt_core_t* jwt_core_new(const char* secret_key, const char* x_vector_hex);
void jwt_core_free(jwt_core_t* core);

/* ------------------------------------------------------------------ */
/* helpers de JSON / base64 / hash (públicos para callers)              */
/* ------------------------------------------------------------------ */

/* Extraen un miembro string/número de un objeto JSON plano. */
int jwt_json_get_string(const char* json, const char* name, char** out);
int jwt_json_get_int(const char* json, const char* name, int64_t* out);

/** Comparación en tiempo constante (crypto.timingSafeEqual). */
bool jwt_safe_compare(const char* a, const char* b);

/** Streebog-256 hex con convenio JS (UTF-16LE). */
char* jwt_core_hash_hex(const char* data);

/** Reversible (CFB) del password con trim, en base64. */
char* jwt_core_generate_password(jwt_core_t* core, const char* password);

/** Verifica password contra su hash Streebog-256 hex. */
bool jwt_core_verify_password(const char* password, const char* stored_hash);

/* ------------------------------------------------------------------ */
/* tokens de bajo nivel                                                 */
/* ------------------------------------------------------------------ */

/**
 * \brief Emite un access token. sign = hash(secret + id + sign_tail); si
 *        sign_tail == NULL se usa cadena vacía. exp_ms es el momento de
 *        expiración (epoch ms); el caller lo calcula desde su config.
 */
char* jwt_core_write_access(jwt_core_t*, const jwt_user_t*, int64_t session_type,
                           int64_t exp_ms, const char* sign_tail);

/** \brief Emite un refresh token (mismo formato, payload sin init). */
char* jwt_core_write_refresh(jwt_core_t*, const jwt_user_t*, int64_t session_type,
                            int64_t exp_ms, const char* sign_tail);

/**
 * \brief Emite un CSRF token (rand 32B hex + expiración).
 *        sign = hash(secret + rand + exp_str + sign_suffix); si
 *        sign_suffix == NULL se usa cadena vacía.
 */
char* jwt_core_write_csrf(jwt_core_t*, int64_t exp_ms,
                         const char* type_or_null, const char* sign_suffix);

/**
 * \brief Descifra y parsea un token. Verifica expiración pero NO
 *        session_type ni sign — el llamador hace ambas para su canal.
 *        NULL si el token es inválido, expirado o no parseable.
 */
jwt_core_payload_t* jwt_core_decode(jwt_core_t*, const char* token);

/**
 * \brief Compara el sign del payload con hash(secret + id + sign_tail)
 *        para ACCESS/REFRESH, o hash(secret + rand + exp + sign_tail)
 *        para CSRF, en tiempo constante.
 */
bool jwt_core_verify_sign(jwt_core_t*, const jwt_core_payload_t*,
                          const char* sign_tail);

/**
 * \brief Acceso único para CSRF: descifra + parse + verifica firma y
 *        expiración.
 */
bool jwt_core_verify_csrf(jwt_core_t*, const char* token,
                          const char* sign_suffix);

void jwt_core_payload_free(jwt_core_payload_t*);

#ifdef __cplusplus
}
#endif

#endif /* JWT_CORE_H */
