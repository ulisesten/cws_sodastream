# feat(kdf): HMAC-Streebog + PBKDF2 (KDF de contraseñas GOST)

## Objetivo

Añadir a la librería las primitivas necesarias para derivar/hashear contraseñas
dentro del estándar GOST:

- **HMAC** sobre GOST R 34.11-2012 (Streebog-256/512), según RFC 7836 /
  R 50.1.113-2016.
- **PBKDF2** (RFC 8018) con `HMAC-Streebog` como PRF, para almacenar
  contraseñas con salt y coste ajustable.

No se pide un KDF memory-hard: **no existe estándar GOST** equivalente a
Argon2/scrypt; no inventar uno.

## Convenciones de la librería a respetar

- Organización **por feature**: `gost/kdf/kdf.h` + `gost/kdf/kdf.c`; incluir en
  `gost/gost.h`.
- **Sin malloc, sin estado global**: tipos por valor, resultados por parámetro
  de salida.
- Toda función fallible devuelve `gost_status_t` (`GOST_OK`, `GOST_ERR_PARAM`,
  `GOST_ERR_STATE`, `GOST_ERR_RNG` de `gost/common.h`).
- Nombres `gost_<feature>_<func>`, enums/macros `gost_*` / `GOST_*`.
- Comentarios Doxygen (`@file`, `@brief`, `@param`, `@return`) como el resto.
- Añadir `gost/kdf/kdf.c` a las fuentes de `CMakeLists.txt`; tests en
  `tests/test_gost.c`; fila en la tabla de correspondencia del `README.md`.

## Presentación del digest (crítico)

La lib tiene dos presentaciones: `gost_hash` one-shot (MSB-first, compatible con
el proyecto JS) y el **streaming** `gost_hash_init/update/final` (RFC 6986,
LSB-first). **HMAC debe usar la presentación RFC 6986 (streaming)** para
coincidir con los vectores de RFC 7836. Documentarlo y **validar con vectores
oficiales** antes de dar por buena la implementación.

## API propuesta (`gost/kdf/kdf.h`)

```c
#include "../common.h"
#include "../hash/hash.h"

#define GOST_HMAC_MAX_DIGEST 64U   /* Streebog-512 */

typedef struct gost_hmac_ctx { /* contenidos privados */ } gost_hmac_ctx_t;

/* HMAC one-shot. width = GOST_HASH_256 | GOST_HASH_512.
 * out debe tener 32 (256) o 64 (512) bytes. */
gost_status_t gost_hmac(const uint8_t *key, size_t key_len,
                        const uint8_t *msg, size_t msg_len,
                        gost_hash_width_t width, uint8_t *out);

/* Variante streaming (recomendada para mensajes grandes). */
gost_status_t gost_hmac_init(gost_hmac_ctx_t *ctx,
                             const uint8_t *key, size_t key_len,
                             gost_hash_width_t width);
gost_status_t gost_hmac_update(gost_hmac_ctx_t *ctx,
                               const uint8_t *data, size_t len);
gost_status_t gost_hmac_final(gost_hmac_ctx_t *ctx, uint8_t *out);

/* PBKDF2 (RFC 8018) con HMAC-Streebog como PRF. */
gost_status_t gost_pbkdf2(const uint8_t *password, size_t password_len,
                          const uint8_t *salt, size_t salt_len,
                          uint32_t iterations,
                          gost_hash_width_t width,
                          uint8_t *out, size_t out_len);
```

## Algoritmo — HMAC (RFC 2104)

- Bloque `B = 64` bytes (Streebog = 512 bits).
- Si `key_len > B`: `K = H(key)` (con el hash del ancho elegido); si no,
  `K = key`.
- Rellenar `K` con ceros hasta `B` → `K0`.
- `ipad = 0x36 × B`, `opad = 0x5C × B`.
- `HMAC(K,m) = H( (K0⊕opad) || H( (K0⊕ipad) || m ) )`.
- Guardas: `key==NULL && key_len>0` → `GOST_ERR_PARAM`; `width` distinto de
  256/512 → `GOST_ERR_PARAM`; `out==NULL` → `GOST_ERR_PARAM`.

## Algoritmo — PBKDF2 (RFC 8018)

- `hLen` = 32 (256) o 64 (512). `dkLen = out_len`.
- `l = ceil(dkLen / hLen)` bloques; `T_i = U_1 ⊕ U_2 ⊕ … ⊕ U_c`, con
  `U_1 = PRF(P, S || INT_BE32(i))`, `U_j = PRF(P, U_{j-1})`, `c = iterations`.
- `i` es 1-based; concatenar `T_i` y truncar a `dkLen`.
- Guardas: `iterations == 0`, `out_len == 0`, `out == NULL`, overflow
  `l > 2^32-1` → `GOST_ERR_PARAM`.
- `password` puede ser `NULL` con `password_len==0` (válido). Lo mismo para
  `salt` si `salt_len==0`.

## Vectores de prueba

- **HMAC-Streebog**: obtener de **RFC 7836, Apéndice A**
  (HMAC_GOSTR3411_2012_256 y _512) y/o R 50.1.113-2016. Fijarlos en
  `tests/test_gost.c`.
- **PBKDF2-Streebog**: no hay vectores en RFC 6070 aplicables (son SHA-1).
  Generar vectores con una implementación independiente acreditada
  (`pygost` / `gostcrypto` / `gost-modes` de Rust) y fijarlos; **no inventar
  valores**.
- Casos de borde: clave 0/1/>64 bytes (incluye el camino `K=H(K)`,
  `iterations=1`, `dkLen` múltiplo y no múltiplo de `hLen`, `salt` vacío,
  `out_len` grande.

## Fuera de alcance (documentar como tal)

- KDF memory-hard: no existe estándar GOST; no implementar ad-hoc.
- KDF de R 50.1.113-2016 (`KDF_GOSTR3411_2012_256`, con `Label`/`Seed`):
  **opcional**. Si se añade, seguir el estándar exacto (es un PRF de una
  pasada, no un KDF de contraseñas por sí solo) y marcarlo aparte.
- Codificación de texto: la contraseña se pasará ya en bytes por el consumidor
  (p. ej. UTF-16LE con `gost_text_utf16le`), igual que el resto de la lib.

## Notas del consumidor (cws_sodastream)

- Se usará `gost_pbkdf2` con **salt aleatorio 16 B por usuario**, **iteraciones
  altas** (a calibrar) y, opcional, **pepper** (secreto del servidor en `.env`,
  prefijado al password antes del KDF).
- Formato de almacenamiento auto-descriptivo propuesto:
  `$gost-pbkdf2$256$<iter>$<salt_b64>$<dk_b64>` (permite subir parámetros).
- Migración: los hashes actuales (`Streebog-256(password)` sin salt) se
  detectan por formato y se re-hashean en el primer login exitoso.
