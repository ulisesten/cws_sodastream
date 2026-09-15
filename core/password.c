/*
 * password.c — hash/verificación de contraseñas con PBKDF2-HMAC-Streebog
 * (feature gost/kdf) + salt aleatorio + pepper del servidor. Fix #2.
 */

#define _POSIX_C_SOURCE 200809L

#include "password.h"

#include <gost/gost.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "configuration.h"     /* cfg_getenv */
#include "jwt_core.h"          /* jwt_core_hash_hex (legacy), jwt_safe_compare */

#define PW_SALT_BYTES 16
#define PW_DK_BYTES   32
#define PW_FORMAT_PREFIX "$gost-pbkdf2$512$"

/* ------------------------------------------------------------------ */
/* hex                                                                 */
/* ------------------------------------------------------------------ */

static void hex_encode(const uint8_t* in, size_t n, char* out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = d[in[i] >> 4];
        out[i * 2 + 1] = d[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

/* Decodifica `in_len` chars hex (deben ser n*2) en n bytes. 0 en éxito. */
static int hex_decode_n(const char* in, size_t in_len, uint8_t* out, size_t n) {
    if (in_len != n * 2) return -1;
    for (size_t i = 0; i < n; i++) {
        int hi = in[i * 2], lo = in[i * 2 + 1];
        hi = (hi >= '0' && hi <= '9') ? hi - '0'
             : (hi >= 'a' && hi <= 'f') ? hi - 'a' + 10
             : (hi >= 'A' && hi <= 'F') ? hi - 'A' + 10 : -1;
        lo = (lo >= '0' && lo <= '9') ? lo - '0'
             : (lo >= 'a' && lo <= 'f') ? lo - 'a' + 10
             : (lo >= 'A' && lo <= 'F') ? lo - 'A' + 10 : -1;
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* parámetros desde la configuración central                           */
/* ------------------------------------------------------------------ */

static uint32_t pw_iterations(void) {
    const char* s = cfg_getenv("PASSWORD_KDF_ITERATIONS");
    if (s && *s) {
        unsigned long v = strtoul(s, NULL, 10);
        if (v >= 1 && v <= 0xFFFFFFFFul) return (uint32_t)v;
    }
    return PASSWORD_DEFAULT_ITER;
}

/* ------------------------------------------------------------------ */
/* derivación: PBKDF2(pepper || UTF-16LE(password), salt)              */
/* ------------------------------------------------------------------ */

/* Deriva PW_DK_BYTES bytes. 0 en éxito. */
static int derive(const char* password, const uint8_t* salt, uint32_t iter,
                  uint8_t out[PW_DK_BYTES]) {
    const char* pepper = cfg_getenv("PASSWORD_PEPPER");
    size_t pepper_len = pepper ? strlen(pepper) : 0;

    /* password en UTF-16LE (mismo convenio que la referencia). */
    size_t plen = strlen(password);
    size_t u16_cap = plen * 2 + 2;
    uint8_t* u16 = (uint8_t*)malloc(u16_cap);
    if (!u16) return -1;
    size_t u16_len = 0;
    if (gost_text_utf16le(password, plen, u16, u16_cap, &u16_len) != GOST_OK) {
        free(u16);
        return -1;
    }

    /* input = pepper || utf16le(password). HMAC comprime claves > 64 B. */
    size_t in_len = pepper_len + u16_len;
    uint8_t* in = (uint8_t*)malloc(in_len ? in_len : 1);
    if (!in) { free(u16); return -1; }
    if (pepper_len) memcpy(in, pepper, pepper_len);
    if (u16_len) memcpy(in + pepper_len, u16, u16_len);
    free(u16);

    gost_status_t st = gost_pbkdf2(in, in_len, salt, PW_SALT_BYTES, iter,
                                   out, PW_DK_BYTES);
    free(in);
    return st == GOST_OK ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

char* password_hash(const char* password) {
    if (!password) return NULL;

    uint8_t salt[PW_SALT_BYTES];
    if (gost_rng_system(NULL, salt, sizeof salt) != GOST_OK) return NULL;

    uint32_t iter = pw_iterations();
    uint8_t dk[PW_DK_BYTES];
    if (derive(password, salt, iter, dk) != 0) return NULL;

    char salt_hex[PW_SALT_BYTES * 2 + 1];
    char dk_hex[PW_DK_BYTES * 2 + 1];
    hex_encode(salt, sizeof salt, salt_hex);
    hex_encode(dk, sizeof dk, dk_hex);

    /* $gost-pbkdf2$512$<iter>$<salt_hex>$<dk_hex> */
    size_t cap = strlen(PW_FORMAT_PREFIX) + 24 + strlen(salt_hex) + 1 +
                 strlen(dk_hex) + 1;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    int n = snprintf(out, cap, "%s%u$%s$%s", PW_FORMAT_PREFIX, iter, salt_hex,
                     dk_hex);
    if (n < 0 || (size_t)n >= cap) { free(out); return NULL; }
    return out;
}

password_result_t password_verify(const char* password, const char* stored) {
    if (!password || !stored || !*stored) return PASSWORD_ERROR;

    /* Formato actual: $gost-pbkdf2$512$<iter>$<salt_hex>$<dk_hex> */
    if (strncmp(stored, PW_FORMAT_PREFIX, strlen(PW_FORMAT_PREFIX)) == 0) {
        const char* rest = stored + strlen(PW_FORMAT_PREFIX);
        char* end = NULL;
        unsigned long iter = strtoul(rest, &end, 10);
        if (end == rest || *end != '$' || iter < 1 || iter > 0xFFFFFFFFul)
            return PASSWORD_ERROR;

        const char* salt_hex = end + 1;
        const char* sep = strchr(salt_hex, '$');
        if (!sep) return PASSWORD_ERROR;
        size_t salt_hex_len = (size_t)(sep - salt_hex);
        const char* dk_hex = sep + 1;

        uint8_t salt[PW_SALT_BYTES];
        uint8_t want[PW_DK_BYTES];
        if (hex_decode_n(salt_hex, salt_hex_len, salt, PW_SALT_BYTES))
            return PASSWORD_ERROR;
        if (hex_decode_n(dk_hex, strlen(dk_hex), want, PW_DK_BYTES))
            return PASSWORD_ERROR;

        uint8_t got[PW_DK_BYTES];
        if (derive(password, salt, (uint32_t)iter, got) != 0)
            return PASSWORD_ERROR;

        /* Comparación en tiempo constante byte a byte. */
        volatile unsigned char diff = 0;
        for (size_t i = 0; i < PW_DK_BYTES; i++) diff |= got[i] ^ want[i];
        return diff == 0 ? PASSWORD_OK : PASSWORD_FAIL;
    }

    /* Legacy: Streebog-256 hex de la contraseña (sin salt). */
    char* legacy = jwt_core_hash_hex(password);
    if (!legacy) return PASSWORD_ERROR;
    bool ok = jwt_safe_compare(legacy, stored);
    free(legacy);
    return ok ? PASSWORD_OK_LEGACY : PASSWORD_FAIL;
}

bool password_needs_rehash(const char* stored) {
    if (!stored) return true;
    if (strncmp(stored, PW_FORMAT_PREFIX, strlen(PW_FORMAT_PREFIX)) != 0)
        return true;   /* legacy */
    const char* rest = stored + strlen(PW_FORMAT_PREFIX);
    char* end = NULL;
    unsigned long iter = strtoul(rest, &end, 10);
    if (end == rest) return true;
    return iter < pw_iterations();
}
