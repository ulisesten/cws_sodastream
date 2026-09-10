/*
 * jwt.c — emisión y verificación de tokens de sesión GOST.
 *
 * Adaptación de reference/jwt.js (class JsonWebToken) sobre el port C de
 * GOST (third_party/c_gost_encryption): cifrado reversible = CFB 28147-89
 * con entrada UTF-16LE y salida base64; hash = Streebog-256 (presentación
 * big-endian, igual que el proyecto JS).
 */

#define _POSIX_C_SOURCE 200809L

#include "jwt.h"

#include <gost/gost.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Tabla de sustitución default del proyecto de referencia
 * (Стандартные_ТЗ[5] = tc26-gost-28147-param-Z). */
#define JWT_SBOX GOST_SBOX_TC26_Z

/** 32 bytes aleatorios -> 64 hex + NUL (crypto.randomBytes(32)). */
#define JWT_RANDOM_BYTES 32

/** Bytes de un componente de la curva (r/s/x/y). */
#define JWT_COMPONENT_BYTES 32

struct jwt {
    uint32_t cipher_key[GOST_CIPHER_KEY_WORDS]; /* X_VECTOR parseado.  */
    uint32_t cipher_iv[2];                      /* IV (SECRET_KEY).    */
    char*    secret;                            /* SECRET_KEY.         */
    int64_t  access_ms;    /* ACCESS_TOKEN_EXPIRATION_MINUTES * 60000. */
    int64_t  refresh_ms;   /* REFRESH_TOKEN_EXPIRATION_DAYS * 86400000. */
};

/* ------------------------------------------------------------------ */
/* utilidades de tiempo                                                */
/* ------------------------------------------------------------------ */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* ------------------------------------------------------------------ */
/* string builder                                                      */
/* ------------------------------------------------------------------ */

typedef struct sbuf {
    char*  data;
    size_t len;
    size_t cap;
} sbuf_t;

static int sbuf_reserve(sbuf_t* sb, size_t extra) {
    if (sb->len + extra + 1 <= sb->cap) return 0;
    size_t ncap = sb->cap ? sb->cap : 128;
    while (ncap < sb->len + extra + 1) ncap *= 2;
    char* nd = (char*)realloc(sb->data, ncap);
    if (!nd) return -1;
    sb->data = nd;
    sb->cap = ncap;
    return 0;
}

static int sbuf_append(sbuf_t* sb, const char* s, size_t n) {
    if (sbuf_reserve(sb, n)) return -1;
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 0;
}

static int sbuf_append_str(sbuf_t* sb, const char* s) {
    return sbuf_append(sb, s, strlen(s));
}

/* Agrega `s` como string JSON (con escapes de ", \ y controles). */
static int sbuf_append_json_str(sbuf_t* sb, const char* s) {
    if (sbuf_append(sb, "\"", 1)) return -1;
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
        char esc[8];
        int n;
        switch (*p) {
            case '"':  n = snprintf(esc, sizeof(esc), "\\\""); break;
            case '\\': n = snprintf(esc, sizeof(esc), "\\\\"); break;
            case '\b': n = snprintf(esc, sizeof(esc), "\\b"); break;
            case '\f': n = snprintf(esc, sizeof(esc), "\\f"); break;
            case '\n': n = snprintf(esc, sizeof(esc), "\\n"); break;
            case '\r': n = snprintf(esc, sizeof(esc), "\\r"); break;
            case '\t': n = snprintf(esc, sizeof(esc), "\\t"); break;
            default:
                if (*p < 0x20) {
                    n = snprintf(esc, sizeof(esc), "\\u%04x", *p);
                } else {
                    if (sbuf_append(sb, (const char*)p, 1)) return -1;
                    continue;
                }
                break;
        }
        if (sbuf_append(sb, esc, (size_t)n)) return -1;
    }
    return sbuf_append(sb, "\"", 1);
}

/* ------------------------------------------------------------------ */
/* base64 / hex                                                        */
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

/* ------------------------------------------------------------------ */
/* UTF-8 <-> UTF-16LE (Код.Строку_в_байты)                             */
/* ------------------------------------------------------------------ */
/* UTF-8 <-> UTF-16LE (Код.Строку_в_байты / Байты_в_строку)             */
/* ------------------------------------------------------------------ */

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
                uint16_t hi = (uint16_t)(0xD800 + (cp >> 10));
                uint16_t lo = (uint16_t)(0xDC00 + (cp & 0x3FF));
                out[o++] = (uint8_t)(hi & 0xFF);
                out[o++] = (uint8_t)(hi >> 8);
                out[o++] = (uint8_t)(lo & 0xFF);
                out[o++] = (uint8_t)(lo >> 8);
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

static char* utf16le_bytes_to_utf8(const uint8_t* in, size_t len) {
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
/* JSON (parser mínimo para los payloads que este módulo genera)        */
/* ------------------------------------------------------------------ */

static const char* json_skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

/* Avanza `p` por completo sobre un valor JSON (string, número, bool, null,
 * objeto o array) y devuelve el puntero justo después del valor. */
static const char* json_skip_value(const char* p) {
    p = json_skip_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1]) p++;
            p++;
        }
        return *p == '"' ? p + 1 : p;
    }
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1]) p++;
                    p++;
                }
                if (!*p) return p;
                p++;
                continue;
            }
            if (*p == open) depth++;
            else if (*p == close) {
                depth--;
                if (depth == 0) return p + 1;
            }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != '\n') p++;
    return p;
}

/* Busca el miembro `name` en el objeto que inicia en `obj` (apunta a '{')
 * y devuelve el puntero al inicio de su valor, o NULL. */
static const char* json_member(const char* obj, const char* name) {
    size_t nlen = strlen(name);
    const char* p = json_skip_ws(obj + 1);
    if (*p == '}') return NULL;
    while (*p) {
        p = json_skip_ws(p);
        if (*p != '"') return NULL;
        const char* key = p + 1;
        /* json_skip_value devuelve el puntero después de la comilla de
         * cierre; la comilla está en key_end-1. */
        const char* key_end = json_skip_value(p);
        if (key_end == key + 1 || key_end[-1] != '"') return NULL;
        size_t klen = (size_t)((key_end - 1) - key);
        p = json_skip_ws(key_end);
        if (*p != ':') return NULL;
        p = json_skip_ws(p + 1);
        const char* value = p;
        p = json_skip_value(p);
        if (klen == nlen && memcmp(key, name, nlen) == 0) return value;
        p = json_skip_ws(p);
        if (*p == ',') {
            p++;
            continue;
        }
        return NULL;
    }
    return NULL;
}

/* Extrae un string JSON como C string malloc. 0 en éxito. */
int json_get_str(const char* json, const char* name, char** out) {
    *out = NULL;
    const char* v = json_member(json, name);
    if (!v || *v != '"') return CWS_ERR_INVALID;
    v++;
    sbuf_t sb = {0};
    while (*v && *v != '"') {
        if (*v == '\\' && v[1]) {
            v++;
            switch (*v) {
                case 'n': sbuf_append(&sb, "\n", 1); break;
                case 't': sbuf_append(&sb, "\t", 1); break;
                case 'r': sbuf_append(&sb, "\r", 1); break;
                case 'b': sbuf_append(&sb, "\b", 1); break;
                case 'f': sbuf_append(&sb, "\f", 1); break;
                case 'u': {
                    unsigned cp = 0;
                    for (int i = 1; i <= 4 && v[i]; i++) {
                        char c = v[i];
                        unsigned d = (c >= '0' && c <= '9') ? (unsigned)(c - '0')
                                   : (c >= 'a' && c <= 'f') ? (unsigned)(c - 'a' + 10)
                                   : (c >= 'A' && c <= 'F') ? (unsigned)(c - 'A' + 10)
                                   : 99;
                        if (d == 99) break;
                        cp = (cp << 4) | d;
                    }
                    if (cp < 0x80) {
                        char c1 = (char)cp;
                        sbuf_append(&sb, &c1, 1);
                    } else if (cp < 0x800) {
                        char seq[2] = {(char)(0xC0 | (cp >> 6)),
                                       (char)(0x80 | (cp & 0x3F))};
                        sbuf_append(&sb, seq, 2);
                    } else {
                        char seq[3] = {(char)(0xE0 | (cp >> 12)),
                                       (char)(0x80 | ((cp >> 6) & 0x3F)),
                                       (char)(0x80 | (cp & 0x3F))};
                        sbuf_append(&sb, seq, 3);
                    }
                    v += 4;
                    break;
                }
                default: sbuf_append(&sb, v, 1); break;
            }
            v++;
        } else {
            sbuf_append(&sb, v, 1);
            v++;
        }
    }
    if (!sb.data) {
        sb.data = (char*)malloc(1);
        if (!sb.data) return CWS_ERR_NOMEM;
        sb.data[0] = '\0';
    }
    *out = sb.data;
    return CWS_OK;
}

int json_get_int(const char* json, const char* name, int64_t* out) {
    const char* v = json_member(json, name);
    if (!v) return CWS_ERR_INVALID;
    *out = (int64_t)strtoll(v, NULL, 10);
    return CWS_OK;
}

/* ------------------------------------------------------------------ */
/* primitivas GOST compartidas                                         */
/* ------------------------------------------------------------------ */

/* Streebog-256 de un texto con el convenio de la referencia JS:
 * el texto se codifica a UTF-16LE (Код.Строку_в_байты) antes de hashear,
 * igual que EncryptService.hash — los digests son idénticos a los del
 * servidor Node (gost/gost/text). */
static char* gost_hash_hex(const char* data) {
    uint8_t digest[GOST_HASH_256 / 8];
    if (gost_hash_text(data, strlen(data), GOST_HASH_256, digest) != GOST_OK)
        return NULL;
    static const char digits[] = "0123456789abcdef";
    char* out = (char*)malloc(sizeof(digest) * 2 + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < sizeof(digest); i++) {
        out[i * 2] = digits[digest[i] >> 4];
        out[i * 2 + 1] = digits[digest[i] & 0x0F];
    }
    out[sizeof(digest) * 2] = '\0';
    return out;
}

/* sign = hash(secret + <usu_id|rand|exp...> + <suffix>) con las piezas
 * concatenadas como en el JS. `middle` es el valor decimal o rand, y
 * `suffix` puede ser NULL. */
static char* payload_sign_hex(const jwt_t* jwt, const char* middle,
                              const char* tail, const char* suffix) {
    sbuf_t sb = {0};
    if (sbuf_append_str(&sb, jwt->secret) ||
        sbuf_append_str(&sb, middle) ||
        (tail && sbuf_append_str(&sb, tail)) ||
        (suffix && sbuf_append_str(&sb, suffix))) {
        free(sb.data);
        return NULL;
    }
    char* hex = gost_hash_hex(sb.data);
    free(sb.data);
    return hex;
}

/* Serializa un payload, lo cifra (UTF-16LE -> CFB) y lo devuelve en base64.
 * `payload_json` ya viene con sign incluido. */
static char* encrypt_payload_b64(const jwt_t* jwt, const sbuf_t* json) {
    size_t plain_bytes = utf8_to_utf16le_bytes(json->data, NULL, 0);
    uint8_t* buf = (uint8_t*)malloc(plain_bytes + 1);
    if (!buf) return NULL;
    utf8_to_utf16le_bytes(json->data, buf, plain_bytes + 1);

    gost_cipher_t cipher;
    if (gost_cipher_init(&cipher, jwt->cipher_key, JWT_SBOX) != GOST_OK ||
        gost_cipher_cfb_encrypt(&cipher, jwt->cipher_iv, buf, plain_bytes) !=
            GOST_OK) {
        free(buf);
        return NULL;
    }
    char* token = b64_encode(buf, plain_bytes);
    free(buf);
    return token;
}

/* Descifra un token base64 y devuelve el JSON en claro malloc. */
static char* decrypt_token_json(const jwt_t* jwt, const char* token) {
    size_t ct_len = 0;
    uint8_t* ct = b64_decode(token, &ct_len);
    if (!ct) return NULL;
    gost_cipher_t cipher;
    if (gost_cipher_init(&cipher, jwt->cipher_key, JWT_SBOX) != GOST_OK ||
        gost_cipher_cfb_decrypt(&cipher, jwt->cipher_iv, ct, ct_len) != GOST_OK) {
        free(ct);
        return NULL;
    }
    char* json = utf16le_bytes_to_utf8(ct, ct_len);
    free(ct);
    return json;
}

/* Serializa el objeto user{...} en `sb` (campos nulos -> null). */
static int append_user_json(sbuf_t* sb, const jwt_user_t* user) {
    char id[24];
    snprintf(id, sizeof(id), "%" PRId64, user->usu_id);
    if (sbuf_append_str(sb, "{\"usu_id\":") ||
        sbuf_append_str(sb, id) ||
        sbuf_append_str(sb, ",\"usu_nombre\":") ||
        sbuf_append_json_str(sb, user->usu_nombre) ||
        sbuf_append_str(sb, ",\"usu_correo\":") ||
        sbuf_append_json_str(sb, user->usu_correo) ||
        sbuf_append_str(sb, ",\"ip\":") ||
        sbuf_append_json_str(sb, user->ip) ||
        sbuf_append_str(sb, ",\"usu_salt\":") ||
        sbuf_append_json_str(sb, user->usu_salt) ||
        sbuf_append_str(sb, "}"))
        return -1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* ciclo de vida                                                       */
/* ------------------------------------------------------------------ */

jwt_t* jwt_new(const app_config_t* cfg) {
    if (!cfg) return NULL;

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

    jwt_t* jwt = (jwt_t*)calloc(1, sizeof(*jwt));
    if (!jwt) return NULL;
    memcpy(jwt->cipher_key, key, sizeof(key));
    for (int i = 0; i < 8 && cfg->secret_key[i]; i++)
        ((uint8_t*)jwt->cipher_iv)[i] = (uint8_t)cfg->secret_key[i];
    jwt->secret = strdup(cfg->secret_key);
    if (!jwt->secret) {
        free(jwt);
        return NULL;
    }
    jwt->access_ms = (int64_t)cfg->access_token_expiration_minutes * 60000LL;
    jwt->refresh_ms = (int64_t)cfg->refresh_token_expiration_days * 86400000LL;
    return jwt;
}

void jwt_free(jwt_t* jwt) {
    if (!jwt) return;
    free(jwt->secret);
    free(jwt);
}

/* ------------------------------------------------------------------ */
/* utilidades públicas                                                 */
/* ------------------------------------------------------------------ */

bool jwt_safe_compare(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return false;
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < la; i++)
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

int jwt_json_get_string(const char* json, const char* name, char** out) {
    if (!json || !name || !out) return CWS_ERR_INVALID;
    return json_get_str(json, name, out);
}

int jwt_json_get_int(const char* json, const char* name, int64_t* out) {
    if (!json || !name || !out) return CWS_ERR_INVALID;
    return json_get_int(json, name, out);
}

/* ------------------------------------------------------------------ */
/* tokens GOST                                                         */
/* ------------------------------------------------------------------ */

char* jwt_write_gost_token(const jwt_t* jwt, const jwt_user_t* user) {
    if (!jwt || !user || !user->ip) return NULL;

    int64_t now = now_ms();
    char id[24], init[24], exp[24];
    snprintf(id, sizeof(id), "%" PRId64, user->usu_id);
    snprintf(init, sizeof(init), "%" PRId64, now);
    snprintf(exp, sizeof(exp), "%" PRId64, now + jwt->access_ms);

    char* sign = payload_sign_hex(jwt, id, user->ip, NULL);
    if (!sign) return NULL;

    sbuf_t sb = {0};
    int ok = sbuf_append_str(&sb, "{\"id\":") == 0 &&
             sbuf_append_str(&sb, id) == 0 &&
             sbuf_append_str(&sb, ",\"init\":") == 0 &&
             sbuf_append_str(&sb, init) == 0 &&
             sbuf_append_str(&sb, ",\"exp\":") == 0 &&
             sbuf_append_str(&sb, exp) == 0 &&
             sbuf_append_str(&sb, ",\"session_type\":") == 0 &&
             sbuf_append_str(&sb, "1") == 0 &&
             sbuf_append_str(&sb, ",\"user\":") == 0 &&
             append_user_json(&sb, user) == 0 &&
             sbuf_append_str(&sb, ",\"sign\":") == 0 &&
             sbuf_append_json_str(&sb, sign) == 0 &&
             sbuf_append_str(&sb, "}") == 0;
    free(sign);
    if (!ok) {
        free(sb.data);
        return NULL;
    }
    char* token = encrypt_payload_b64(jwt, &sb);
    free(sb.data);
    return token;
}

static jwt_gost_payload_t* verify_gost_token(const jwt_t* jwt,
                                             const char* token,
                                             int is_refresh) {
    if (!jwt || !token) return NULL;

    char* json = decrypt_token_json(jwt, token);
    if (!json) return NULL;

    jwt_gost_payload_t* p =
        (jwt_gost_payload_t*)calloc(1, sizeof(*p));
    if (!p) {
        free(json);
        return NULL;
    }

    char* expected = NULL;
    int64_t today = now_ms();

    if (json_get_int(json, "id", &p->id) != CWS_OK ||
        json_get_int(json, "exp", &p->exp) != CWS_OK ||
        json_get_int(json, "session_type", &p->session_type) != CWS_OK) {
        free(json);
        jwt_payload_free(p);
        return NULL;
    }

    const char* user_obj = json_member(json, "user");
    int user_ok = user_obj != NULL;
    if (user_ok)
        user_ok = json_get_int(user_obj, "usu_id", &p->user.usu_id) == CWS_OK;
    if (user_ok)
        user_ok = json_get_str(user_obj, "usu_correo",
                               &p->user.usu_correo) == CWS_OK;
    if (user_ok)
        user_ok = json_get_str(user_obj, "usu_nombre",
                               &p->user.usu_nombre) == CWS_OK;
    if (user_ok) user_ok = json_get_str(user_obj, "ip", &p->user.ip) == CWS_OK;
    if (user_ok)
        user_ok = json_get_str(user_obj, "usu_salt",
                               &p->user.usu_salt) == CWS_OK;
    if (user_ok)
        user_ok = json_get_str(json, "sign", &p->sign) == CWS_OK;

    char id[24];
    snprintf(id, sizeof(id), "%" PRId64, p->id);
    /* Access: sign = hash(secret + id + ip). Refresh:
     * sign = hash(secret + id + 'refresh'). */
    expected = is_refresh ? payload_sign_hex(jwt, id, "refresh", NULL)
                          : payload_sign_hex(jwt, id, p->user.ip, NULL);

    /* Orden de validación de la referencia: firma, expiración,
     * session_type. Cualquier fallo -> NULL. */
    if (!user_ok || !expected ||
        !jwt_safe_compare(p->sign, expected) ||
        today > p->exp ||
        p->session_type != JWT_WEB_SESSION_TYPE) {
        free(expected);
        free(json);
        jwt_payload_free(p);
        return NULL;
    }

    free(expected);
    free(json);
    return p;
}

char* jwt_write_refresh_token(const jwt_t* jwt, const jwt_user_t* user) {
    if (!jwt || !user) return NULL;

    int64_t now = now_ms();
    char id[24], exp[24];
    snprintf(id, sizeof(id), "%" PRId64, user->usu_id);
    snprintf(exp, sizeof(exp), "%" PRId64, now + jwt->refresh_ms);

    char* sign = payload_sign_hex(jwt, id, "refresh", NULL);
    if (!sign) return NULL;

    sbuf_t sb = {0};
    int ok = sbuf_append_str(&sb, "{\"id\":") == 0 &&
             sbuf_append_str(&sb, id) == 0 &&
             sbuf_append_str(&sb, ",\"exp\":") == 0 &&
             sbuf_append_str(&sb, exp) == 0 &&
             sbuf_append_str(&sb, ",\"type\":\"refresh\",\"session_type\":") == 0 &&
             sbuf_append_str(&sb, "1") == 0 &&
             sbuf_append_str(&sb, ",\"user\":") == 0 &&
             append_user_json(&sb, user) == 0 &&
             sbuf_append_str(&sb, ",\"sign\":") == 0 &&
             sbuf_append_json_str(&sb, sign) == 0 &&
             sbuf_append_str(&sb, "}") == 0;
    free(sign);
    if (!ok) {
        free(sb.data);
        return NULL;
    }
    char* token = encrypt_payload_b64(jwt, &sb);
    free(sb.data);
    return token;
}

jwt_gost_payload_t* jwt_gost_verify(const jwt_t* jwt, const char* token) {
    return verify_gost_token(jwt, token, 0);
}

jwt_gost_payload_t* jwt_verify_refresh_token(const jwt_t* jwt,
                                             const char* token) {
    return verify_gost_token(jwt, token, 1);
}

void jwt_payload_free(jwt_gost_payload_t* payload) {
    if (!payload) return;
    free(payload->user.usu_nombre);
    free(payload->user.usu_correo);
    free(payload->user.ip);
    free(payload->user.usu_salt);
    free(payload->sign);
    free(payload);
}

/* ------------------------------------------------------------------ */
/* CSRF tokens                                                         */
/* ------------------------------------------------------------------ */

/* Emite un token CSRF: rand 32 bytes hex; el de sesión vive 2 h y el de
 * refresh REFRESH_TOKEN_EXPIRATION_DAYS. sign = hash(secret + rand + exp
 * [+ 'refresh_csrf']). */
static char* write_csrf(const jwt_t* jwt, const char* type,
                        const char* sign_suffix) {
    if (!jwt) return NULL;

    uint8_t rand_bytes[JWT_RANDOM_BYTES];
    if (gost_rng_system(NULL, rand_bytes, sizeof(rand_bytes)) != GOST_OK)
        return NULL;
    char rand_hex[JWT_RANDOM_HEX + 1];
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < JWT_RANDOM_BYTES; i++) {
        rand_hex[i * 2] = digits[rand_bytes[i] >> 4];
        rand_hex[i * 2 + 1] = digits[rand_bytes[i] & 0x0F];
    }
    rand_hex[JWT_RANDOM_HEX] = '\0';

    int64_t exp = now_ms() + (type ? jwt->refresh_ms : 2LL * 60 * 60 * 1000);
    char exp_str[24];
    snprintf(exp_str, sizeof(exp_str), "%" PRId64, exp);

    /* sign = hash(secret + rand + exp [+ 'refresh_csrf']) */
    sbuf_t sb = {0};
    if (sbuf_append_str(&sb, jwt->secret) ||
        sbuf_append_str(&sb, rand_hex) ||
        sbuf_append_str(&sb, exp_str) ||
        (sign_suffix && sbuf_append_str(&sb, sign_suffix))) {
        free(sb.data);
        return NULL;
    }
    char* sign = gost_hash_hex(sb.data);
    free(sb.data);
    if (!sign) return NULL;

    sbuf_t payload = {0};
    int ok = sbuf_append_str(&payload, "{\"exp\":") == 0 &&
             sbuf_append_str(&payload, exp_str) == 0 &&
             sbuf_append_str(&payload, ",\"rand\":") == 0 &&
             sbuf_append_json_str(&payload, rand_hex) == 0 &&
             (type ? sbuf_append_str(&payload, ",\"type\":") == 0 &&
                     sbuf_append_json_str(&payload, type) == 0
                   : 1) &&
             sbuf_append_str(&payload, ",\"sign\":") == 0 &&
             sbuf_append_json_str(&payload, sign) == 0 &&
             sbuf_append_str(&payload, "}") == 0;
    free(sign);
    if (!ok) {
        free(payload.data);
        return NULL;
    }
    char* token = encrypt_payload_b64(jwt, &payload);
    free(payload.data);
    return token;
}

char* jwt_write_csrf_token(const jwt_t* jwt) {
    return write_csrf(jwt, NULL, NULL);
}

char* jwt_write_refresh_csrf_token(const jwt_t* jwt) {
    return write_csrf(jwt, "refresh_csrf", "refresh_csrf");
}

bool jwt_verify_csrf_token(const jwt_t* jwt, const char* token) {
    if (!jwt || !token) return false;

    char* json = decrypt_token_json(jwt, token);
    if (!json) return false;

    char* rand_hex = NULL;
    char* type = NULL;
    char* sign = NULL;
    int64_t exp = 0;

    int rc = json_get_int(json, "exp", &exp);
    if (rc == CWS_OK) rc = json_get_str(json, "rand", &rand_hex);
    /* "type" solo existe en el CSRF de refresh; en el de sesión está
     * ausente y no es error. */
    if (rc == CWS_OK) json_get_str(json, "type", &type);
    if (rc == CWS_OK) rc = json_get_str(json, "sign", &sign);

    bool valid = false;
    if (rc == CWS_OK && rand_hex && sign) {
        int64_t today = now_ms();
        const char* suffix = (type && !strcmp(type, "refresh_csrf"))
                                 ? "refresh_csrf"
                                 : NULL;
        char exp_str[24];
        snprintf(exp_str, sizeof(exp_str), "%" PRId64, exp);

        sbuf_t sb = {0};
        if (sbuf_append_str(&sb, jwt->secret) == 0 &&
            sbuf_append_str(&sb, rand_hex) == 0 &&
            sbuf_append_str(&sb, exp_str) == 0 &&
            (suffix ? sbuf_append_str(&sb, suffix) == 0 : 1)) {
            char* expected = gost_hash_hex(sb.data);
            valid = expected && jwt_safe_compare(expected, sign) &&
                    today <= exp;
            free(expected);
        }
        free(sb.data);
    }

    free(rand_hex);
    free(type);
    free(sign);
    free(json);
    return valid;
}

/* ------------------------------------------------------------------ */
/* contraseñas                                                         */
/* ------------------------------------------------------------------ */

char* jwt_hash_hex(const char* data) {
    return data ? gost_hash_hex(data) : NULL;
}

char* jwt_generate_hash(const jwt_t* jwt, const char* password) {
    if (!jwt || !password) return NULL;

    /* password.trim() */
    while (*password == ' ' || *password == '\t') password++;
    size_t len = strlen(password);
    while (len > 0 && (password[len - 1] == ' ' || password[len - 1] == '\t'))
        len--;

    size_t plain_bytes = utf8_to_utf16le_bytes(password, NULL, 0);
    uint8_t* buf = (uint8_t*)malloc(plain_bytes + 1);
    if (!buf) return NULL;
    size_t used = utf8_to_utf16le_bytes(password, buf, plain_bytes + 1);
    (void)len;

    gost_cipher_t cipher;
    if (gost_cipher_init(&cipher, jwt->cipher_key, JWT_SBOX) != GOST_OK ||
        gost_cipher_cfb_encrypt(&cipher, jwt->cipher_iv, buf, used) != GOST_OK) {
        free(buf);
        return NULL;
    }
    char* token = b64_encode(buf, used);
    free(buf);
    return token;
}

bool jwt_gost_hash_verify(const jwt_t* jwt, const char* password,
                          const char* hash) {
    if (!jwt || !password || !hash) return false;
    char* computed = gost_hash_hex(password);
    if (!computed) return false;
    bool ok = strlen(computed) == strlen(hash) &&
              jwt_safe_compare(computed, hash);
    free(computed);
    return ok;
}
