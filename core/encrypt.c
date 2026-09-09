/*
 * encrypt.c — servicio criptográfico GOST para cws_sodastream.
 *
 * Adaptación de reference/nodejs/encrypt.js (EncryptService) del submódulo
 * third_party/c_gost_encryption. Usa la librería estática `gost`.
 */

#define _POSIX_C_SOURCE 200809L

#include "encrypt.h"

#include <gost/gost.h>

#include <stdlib.h>
#include <string.h>

/* Curva y tabla por defecto de la referencia JS:
 *   Наборы_параметров[4]  = GostR3410-2001-CryptoPro-C-ParamSet
 *   Стандартные_ТЗ[5]     = tc26-gost-28147-param-Z                    */
#define ENCRYPT_CURVE GOST_CURVE_2001_CRYPTOPRO_C
#define ENCRYPT_SBOX  GOST_SBOX_TC26_Z

/** Tamaño (bytes) de los componentes r/s y de las coordenadas de la curva. */
#define ENCRYPT_COMPONENT_BYTES 32

struct encrypt_service {
    uint32_t cipher_key[GOST_CIPHER_KEY_WORDS]; /* X_VECTOR parseado.        */
    uint32_t cipher_iv[2];                      /* IV desde SECRET_KEY.      */
    gost_key_pair_t keys;                       /* par de firma generado.    */
};

/* ------------------------------------------------------------------ */
/* utilidades: base64, hex, UTF-8 <-> UTF-16LE                         */
/* ------------------------------------------------------------------ */

static const char b64_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char* b64_encode(const uint8_t* in, size_t len) {
    size_t out_len = ((len + 2) / 3) * 4;
    char* out = (char*)malloc(out_len + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int rem = (int)(len - i);
        if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
        if (rem > 2) v |= in[i + 2];
        out[o++] = b64_chars[(v >> 18) & 0x3F];
        out[o++] = b64_chars[(v >> 12) & 0x3F];
        out[o++] = rem > 1 ? b64_chars[(v >> 6) & 0x3F] : '=';
        out[o++] = rem > 2 ? b64_chars[v & 0x3F] : '=';
    }
    out[o] = '\0';
    return out;
}

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static uint8_t* b64_decode(const char* in, size_t* out_len) {
    size_t in_len = strlen(in);
    while (in_len > 0 && in[in_len - 1] == '=') in_len--;
    uint8_t* out = (uint8_t*)malloc((in_len * 3) / 4 + 4);
    if (!out) return NULL;
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < in_len; i++) {
        int v = b64_decode_char(in[i]);
        if (v < 0) {
            free(out);
            return NULL;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }
    *out_len = o;
    return out;
}

static char* hex_encode(const uint8_t* in, size_t len) {
    static const char digits[] = "0123456789abcdef";
    char* out = (char*)malloc(len * 2 + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
    return out;
}

static int hex_decode_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char* in, uint8_t* out, size_t out_cap,
                      size_t* out_len) {
    size_t in_len = strlen(in);
    if (in_len % 2 != 0 || in_len / 2 > out_cap) return CWS_ERR_INVALID;
    for (size_t i = 0; i < in_len; i += 2) {
        int hi = hex_decode_nibble(in[i]);
        int lo = hex_decode_nibble(in[i + 1]);
        if (hi < 0 || lo < 0) return CWS_ERR_INVALID;
        out[i / 2] = (uint8_t)((hi << 4) | lo);
    }
    *out_len = in_len / 2;
    return CWS_OK;
}

/* UTF-8 -> UTF-16LE (como Код.Строку_в_байты: cada char -> uint16 LE).
 * Con out == NULL calcula la longitud necesaria en bytes. */
static size_t utf8_to_utf16le_bytes(const char* s, uint8_t* out, size_t cap) {
    size_t in_len = strlen(s);
    size_t i = 0, o = 0;
    while (i < in_len) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) {
            cp = c;
            n = 1;
        } else if ((c >> 5) == 0x6 && i + 1 < in_len) {
            cp = c & 0x1F;
            n = 2;
        } else if ((c >> 4) == 0xE && i + 2 < in_len) {
            cp = c & 0x0F;
            n = 3;
        } else if ((c >> 3) == 0x1E && i + 3 < in_len) {
            cp = c & 0x07;
            n = 4;
        } else {
            i++;
            continue;
        }
        for (size_t k = 1; k < n; k++)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += n;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            if (out && o + 4 <= cap) {
                out[o++] = (uint8_t)(0xD800 + (cp >> 10));
                out[o++] = (uint8_t)((0xD800 + (cp >> 10)) >> 8);
                out[o++] = (uint8_t)(0xDC00 + (cp & 0x3FF));
                out[o++] = (uint8_t)((0xDC00 + (cp & 0x3FF)) >> 8);
            } else {
                o += 4;
            }
        } else {
            if (out && o + 2 <= cap) {
                out[o++] = (uint8_t)(cp & 0xFF);
                out[o++] = (uint8_t)(cp >> 8);
            } else {
                o += 2;
            }
        }
    }
    return o;
}

/* UTF-16LE -> UTF-8 (como Код.Байты_в_строку). NULL si no hay memoria. */
static char* utf16le_bytes_to_utf8(const uint8_t* in, size_t len) {
    /* UTF-16LE -> UTF-8 en dos pasadas: primero unidades (con surrogates),
     * luego serialización. */
    size_t units = len / 2;
    size_t bytes = 0;
    for (size_t i = 0; i < units; i++) {
        uint32_t cp = (uint32_t)in[i * 2] | ((uint32_t)in[i * 2 + 1] << 8);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units) {
            uint32_t lo = (uint32_t)in[(i + 1) * 2] |
                          ((uint32_t)in[(i + 1) * 2 + 1] << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (cp < 0x80) bytes += 1;
        else if (cp < 0x800) bytes += 2;
        else if (cp < 0x10000) bytes += 3;
        else bytes += 4;
    }
    char* out = (char*)malloc(bytes + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < units; i++) {
        uint32_t cp = (uint32_t)in[i * 2] | ((uint32_t)in[i * 2 + 1] << 8);
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units) {
            uint32_t lo = (uint32_t)in[(i + 1) * 2] |
                          ((uint32_t)in[(i + 1) * 2 + 1] << 8);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* construcción / destrucción                                          */
/* ------------------------------------------------------------------ */

encrypt_service_t* encrypt_service_new(const app_config_t* cfg) {
    if (!cfg) return NULL;

    /* Clave: X_VECTOR = 8 palabras hex separadas por coma. */
    uint32_t key[GOST_CIPHER_KEY_WORDS] = {0};
    {
        const char* p = cfg->x_vector;
        int word = 0;
        while (*p && word < GOST_CIPHER_KEY_WORDS) {
            char* end = NULL;
            unsigned long v = strtoul(p, &end, 16);
            if (end == p) return NULL;
            key[word++] = (uint32_t)v;
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
        if (word != GOST_CIPHER_KEY_WORDS) return NULL;
    }

    /* IV: primeros 8 bytes de SECRET_KEY en little-endian; {0,0} si vacía. */
    uint32_t iv[2] = {0, 0};
    {
        const char* s = cfg->secret_key;
        for (int i = 0; i < 8 && s[i]; i++)
            ((uint8_t*)iv)[i] = (uint8_t)s[i];
    }

    encrypt_service_t* es = (encrypt_service_t*)calloc(1, sizeof(*es));
    if (!es) return NULL;
    memcpy(es->cipher_key, key, sizeof(key));
    memcpy(es->cipher_iv, iv, sizeof(iv));

    const gost_curve_t* curve = gost_curve(ENCRYPT_CURVE);
    if (!curve ||
        gost_signature_generate_keys(curve, gost_rng_system, NULL,
                                     &es->keys) != GOST_OK) {
        free(es);
        return NULL;
    }
    return es;
}

void encrypt_service_free(encrypt_service_t* es) {
    free(es);
}

/* ------------------------------------------------------------------ */
/* firma digital (ЭЦП)                                                 */
/* ------------------------------------------------------------------ */

int encrypt_sign(encrypt_service_t* es, const char* data,
                 char* out, size_t out_cap) {
    if (!es || !data || !out || out_cap < ENCRYPT_SIGNATURE_HEX + 1)
        return CWS_ERR_INVALID;

    const gost_curve_t* curve = gost_curve(ENCRYPT_CURVE);
    if (!curve) return CWS_ERR_GENERIC;

    gost_signature_t sig;
    if (gost_signature_sign_message(curve, &es->keys.private_key,
                                    (const uint8_t*)data, strlen(data),
                                    gost_rng_system, NULL, &sig) != GOST_OK)
        return CWS_ERR_GENERIC;

    uint8_t r[ENCRYPT_COMPONENT_BYTES];
    uint8_t s[ENCRYPT_COMPONENT_BYTES];
    gost_u512_to_bytes_be(sig.r, r, sizeof(r));
    gost_u512_to_bytes_be(sig.s, s, sizeof(s));

    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < ENCRYPT_COMPONENT_BYTES; i++) {
        out[i * 2]       = digits[r[i] >> 4];
        out[i * 2 + 1]   = digits[r[i] & 0x0F];
        out[64 + i * 2]  = digits[s[i] >> 4];
        out[64 + i * 2 + 1] = digits[s[i] & 0x0F];
    }
    out[ENCRYPT_SIGNATURE_HEX] = '\0';
    return CWS_OK;
}

int encrypt_verify(encrypt_service_t* es, const char* data,
                   const char* signature_hex, bool* valid) {
    if (!es || !data || !signature_hex || !valid) return CWS_ERR_INVALID;
    *valid = false;

    uint8_t raw[ENCRYPT_COMPONENT_BYTES * 2];
    size_t raw_len = 0;
    if (hex_decode(signature_hex, raw, sizeof(raw), &raw_len) != CWS_OK ||
        raw_len != sizeof(raw))
        return CWS_ERR_INVALID;

    const gost_curve_t* curve = gost_curve(ENCRYPT_CURVE);
    if (!curve) return CWS_ERR_GENERIC;

    gost_signature_t sig;
    if (gost_u512_from_bytes_be(raw, ENCRYPT_COMPONENT_BYTES, &sig.r) != GOST_OK ||
        gost_u512_from_bytes_be(raw + ENCRYPT_COMPONENT_BYTES,
                                ENCRYPT_COMPONENT_BYTES, &sig.s) != GOST_OK)
        return CWS_ERR_INVALID;

    if (gost_signature_verify_message(curve, &es->keys.public_key,
                                      (const uint8_t*)data, strlen(data),
                                      &sig, valid) != GOST_OK)
        return CWS_ERR_GENERIC;
    return CWS_OK;
}

int encrypt_public_key(encrypt_service_t* es, char* out, size_t out_cap) {
    if (!es || !out || out_cap < ENCRYPT_PUBKEY_HEX + 1)
        return CWS_ERR_INVALID;
    uint8_t x[ENCRYPT_COMPONENT_BYTES];
    uint8_t y[ENCRYPT_COMPONENT_BYTES];
    gost_u512_to_bytes_be(es->keys.public_key.x, x, sizeof(x));
    gost_u512_to_bytes_be(es->keys.public_key.y, y, sizeof(y));
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < ENCRYPT_COMPONENT_BYTES; i++) {
        out[i * 2]     = digits[x[i] >> 4];
        out[i * 2 + 1] = digits[x[i] & 0x0F];
        out[64 + i * 2] = digits[y[i] >> 4];
        out[64 + i * 2 + 1] = digits[y[i] & 0x0F];
    }
    out[ENCRYPT_PUBKEY_HEX] = '\0';
    return CWS_OK;
}

/* ------------------------------------------------------------------ */
/* cifrado reversible (Шифрование)                                     */
/* ------------------------------------------------------------------ */

char* encrypt_reversible(encrypt_service_t* es, const char* data) {
    if (!es || !data) return NULL;

    size_t plain_bytes = utf8_to_utf16le_bytes(data, NULL, 0);
    uint8_t* buf = (uint8_t*)malloc(plain_bytes + 1);
    if (!buf) return NULL;
    utf8_to_utf16le_bytes(data, buf, plain_bytes + 1);

    gost_cipher_t cipher;
    if (gost_cipher_init(&cipher, es->cipher_key, ENCRYPT_SBOX) != GOST_OK ||
        gost_cipher_cfb_encrypt(&cipher, es->cipher_iv, buf, plain_bytes) !=
            GOST_OK) {
        free(buf);
        return NULL;
    }

    char* token = b64_encode(buf, plain_bytes);
    free(buf);
    return token;
}

char* encrypt_decrypt(encrypt_service_t* es, const char* token) {
    if (!es || !token) return NULL;

    size_t ct_len = 0;
    uint8_t* ct = b64_decode(token, &ct_len);
    if (!ct) return NULL;

    gost_cipher_t cipher;
    if (gost_cipher_init(&cipher, es->cipher_key, ENCRYPT_SBOX) != GOST_OK ||
        gost_cipher_cfb_decrypt(&cipher, es->cipher_iv, ct, ct_len) != GOST_OK) {
        free(ct);
        return NULL;
    }

    char* plain = utf16le_bytes_to_utf8(ct, ct_len);
    free(ct);
    return plain;
}

/* ------------------------------------------------------------------ */
/* hash (Хэшевание)                                                    */
/* ------------------------------------------------------------------ */

char* encrypt_hash(encrypt_service_t* es, const char* data) {
    if (!es || !data) return NULL;
    uint8_t digest[GOST_HASH_256 / 8];
    if (gost_hash((const uint8_t*)data, strlen(data), GOST_HASH_256,
                  digest) != GOST_OK)
        return NULL;
    return hex_encode(digest, sizeof(digest));
}
