#ifndef ENCRYPT_H
#define ENCRYPT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "cws/cws.h"
#include "configuration.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * EncryptService — servicio criptográfico GOST.
 *
 * Adaptación en C de reference/nodejs/encrypt.js (class EncryptService) del
 * submódulo third_party/c_gost_encryption, sobre la librería `gost`:
 *
 *   encrypt.js                        encrypt.c
 *   ───────────────────────────────   ─────────────────────────────────────────
 *   gost.ЭЦП.Подписать(data, d)    →  encrypt_sign()    (firma hex r||s)
 *   gost.ЭЦП.Проверить(data, f, Q) →  encrypt_verify()
 *   Шифрование.Гаммование_с_       →  encrypt_reversible()  (CFB + base64)
 *     обратной_связью(..., false)
 *   Шифрование.Гаммование_...(..., →  encrypt_decrypt()
 *     true) + base64
 *   gost.Хэшевание.Вычислить(...)  →  encrypt_hash()        (Streebog-256 hex)
 *
 * Parámetros (centralizados en core/configuration):
 *   - Clave del cifrado: X_VECTOR — 8 palabras hex separadas por coma
 *     ("0x33206D54,0x326C6568,..."), tabla de sustitución TC26-Z (la
 *     default del proyecto de referencia, índice 5).
 *   - IV del modo CFB: primeros 8 bytes de SECRET_KEY en little-endian;
 *     {0,0} si SECRET_KEY viene vacío.
 *   - Firma: curva GostR3410-2001-CryptoPro-C (default de la referencia,
 *     índice 4); el par de claves se genera al construir el servicio.
 *   - Entrada del cifrado codificada UTF-16LE (como Код.Строку_в_байты).
 *
 * NOTA de interoperabilidad: el JS de referencia pasa SECRET (cadena) donde
 * la librería espera la tabla de sustitución, por lo que sus tokens no son
 * reproducibles; los tokens de este módulo solo son interoperables entre
 * instancias C con la misma configuración.
 */

/** Longitud hex de una firma (r+s, 32 bytes c/u) sin el NUL final. */
#define ENCRYPT_SIGNATURE_HEX 128
/** Longitud hex del hash Streebog-256 sin el NUL final. */
#define ENCRYPT_HASH_HEX 64
/** Longitud hex de la clave pública (x||y, 32 bytes c/u) sin el NUL final. */
#define ENCRYPT_PUBKEY_HEX 128

typedef struct encrypt_service encrypt_service_t;

/**
 * \brief Construye el servicio a partir de la configuración central.
 *
 * Parsea X_VECTOR como clave, deriva el IV de SECRET_KEY y genera el par de
 * claves de firma (gost_rng_system). NULL si falta configuración, si
 * X_VECTOR no tiene 8 palabras válidas o si falla la generación de claves.
 *
 * \param[in] cfg configuración (puede liberarse después; se copia).
 */
encrypt_service_t* encrypt_service_new(const app_config_t* cfg);

/** Libera el servicio. */
void encrypt_service_free(encrypt_service_t* es);

/**
 * \brief Firma un mensaje: equivalente a gost.ЭЦП.Подписать(data, d).
 *
 * \param[in]  es      servicio.
 * \param[in]  data    mensaje (UTF-8).
 * \param[out] out     buffer de salida, hex "r||s" + NUL.
 * \param[in]  out_cap capacidad de out; >= ENCRYPT_SIGNATURE_HEX + 1.
 * \return CWS_OK, CWS_ERR_INVALID (buffer/capacidad) o CWS_ERR_GENERIC.
 */
int encrypt_sign(encrypt_service_t* es, const char* data,
                 char* out, size_t out_cap);

/**
 * \brief Verifica una firma: equivalente a gost.ЭЦП.Проверить(data, f, Q).
 *
 * \param[in]  es            servicio (usa su clave pública).
 * \param[in]  data          mensaje (UTF-8).
 * \param[in]  signature_hex firma hex "r||s" producida por encrypt_sign().
 * \param[out] valid         true si la firma corresponde al mensaje.
 * \return CWS_OK, o CWS_ERR_INVALID si signature_hex es inválida.
 */
int encrypt_verify(encrypt_service_t* es, const char* data,
                   const char* signature_hex, bool* valid);

/**
 * \brief Clave pública del par generado, hex "x||y" big-endian.
 *
 * Útil para publicarla y que otros verifiquen las firmas del servicio.
 * out_cap >= ENCRYPT_PUBKEY_HEX + 1.
 */
int encrypt_public_key(encrypt_service_t* es, char* out, size_t out_cap);

/**
 * \brief Cifrado reversible: equivalente a reversible_encrypt(data).
 *
 * Codifica data como UTF-16LE, cifra con GOST 28147-89 modo CFB (clave
 * X_VECTOR, IV de SECRET_KEY, tabla TC26-Z) y devuelve el token en base64.
 *
 * \return buffer malloc con el token base64 (liberar con free), o NULL.
 */
char* encrypt_reversible(encrypt_service_t* es, const char* data);

/**
 * \brief Descifra un token: equivalente a decrypt(texto_cifrado).
 *
 * Decodifica base64, descifra CFB y decodifica UTF-16LE → UTF-8.
 * \return buffer malloc con el texto claro (liberar con free), o NULL.
 */
char* encrypt_decrypt(encrypt_service_t* es, const char* token);

/**
 * \brief Hash Streebog-256 en hex: equivalente a gost.Хэшевание.Вычислить.
 * \return buffer malloc con 64 chars hex + NUL (liberar con free), o NULL.
 */
char* encrypt_hash(encrypt_service_t* es, const char* data);

#ifdef __cplusplus
}
#endif

#endif /* ENCRYPT_H */
