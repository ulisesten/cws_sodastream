/*
 * jwt_core.c — primitivas compartidas entre jwt (web) y jwt_mobile.
 * Bajo nivel: contexto criptográfico, helpers (base64/hex/utf16le/json),
 * emisión y verificación de tokens (access/refresh/CSRF) con sign_tail
 * y session_type configurables.
 */

#define _POSIX_C_SOURCE 200809L

#include "jwt_core.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct jwt_core {
    char*    secret;
    uint32_t cipher_key[GOST_CIPHER_KEY_WORDS];
    uint32_t cipher_iv[2];
};

/* --- tiempo + UTF-8/UTF-16LE ------------------------------------------ */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

/* --- base64 / hex ------------------------------------------------------ */

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
        if (v < 0) { free(out); return NULL; }
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

static void hex_encode(const uint8_t* in, size_t len, char* out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = digits[in[i] >> 4];
        out[i * 2 + 1] = digits[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

/* --- UTF-8 <-> UTF-16LE ----------------------------------------------- */

static size_t utf8_to_utf16le(const char* s, size_t len, uint8_t* out,
                             size_t cap) {
    size_t i = 0, o = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 0x6 && i + 1 < len) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 0xE && i + 2 < len) { cp = c & 0x0F; n = 3; }
        else if ((c >> 3) == 0x1E && i + 3 < len) { cp = c & 0x07; n = 4; }
        else { i++; continue; }
        for (size_t k = 1; k < n; k++)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += n;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            if (out && o + 4 <= cap) {
                uint16_t hi = (uint16_t)(0xD800 + (cp >> 10));
                uint16_t lo = (uint16_t)(0xDC00 + (cp & 0x3FF));
                out[o++] = (uint8_t)(hi & 0xFF); out[o++] = (uint8_t)(hi >> 8);
                out[o++] = (uint8_t)(lo & 0xFF); out[o++] = (uint8_t)(lo >> 8);
            } else o += 4;
        } else {
            if (out && o + 2 <= cap) {
                out[o++] = (uint8_t)(cp & 0xFF);
                out[o++] = (uint8_t)(cp >> 8);
            } else o += 2;
        }
    }
    return o;
}

static char* utf16le_to_utf8(const uint8_t* in, size_t len) {
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
        if (cp < 0x80) out[o++] = (char)cp;
        else if (cp < 0x800) {
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

/* --- string builder + JSON helpers ---------------------------------- */

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
    sb->data = nd; sb->cap = ncap; return 0;
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

static int sbuf_append_json_str(sbuf_t* sb, const char* s) {
    if (sbuf_append(sb, "\"", 1)) return -1;
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; p++) {
        char esc[8];
        int n;
        switch (*p) {
            case '"':  n = snprintf(esc, sizeof esc, "\\\""); break;
            case '\\': n = snprintf(esc, sizeof esc, "\\\\"); break;
            case '\b': n = snprintf(esc, sizeof esc, "\\b"); break;
            case '\f': n = snprintf(esc, sizeof esc, "\\f"); break;
            case '\n': n = snprintf(esc, sizeof esc, "\\n"); break;
            case '\r': n = snprintf(esc, sizeof esc, "\\r"); break;
            case '\t': n = snprintf(esc, sizeof esc, "\\t"); break;
            default:
                if (*p < 0x20) {
                    n = snprintf(esc, sizeof esc, "\\u%04x", *p);
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

static const char* json_skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char* json_skip_value(const char* p) {
    p = json_skip_ws(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
        return *p == '"' ? p + 1 : p;
    }
    if (*p == '{' || *p == '[') {
        char open = *p, close = (open == '{') ? '}' : ']';
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p++;
                while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
                if (!*p) return p;
                p++; continue;
            }
            if (*p == open) depth++;
            else if (*p == close) { depth--; if (depth == 0) return p + 1; }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != '\n') p++;
    return p;
}

static const char* json_member(const char* obj, const char* name) {
    size_t nlen = strlen(name);
    const char* p = json_skip_ws(obj + 1);
    if (*p == '}') return NULL;
    while (*p) {
        p = json_skip_ws(p);
        if (*p != '"') return NULL;
        const char* key = p + 1;
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
        if (*p == ',') { p++; continue; }
        return NULL;
    }
    return NULL;
}

static int json_get_str(const char* json, const char* name, char** out) {
    *out = NULL;
    const char* v = json_member(json, name);
    if (!v || *v != '"') return -1;
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
        if (!sb.data) return -2;
        sb.data[0] = '\0';
    }
    *out = sb.data;
    return 0;
}

static int json_get_int(const char* json, const char* name, int64_t* out) {
    const char* v = json_member(json, name);
    if (!v) return -1;
    *out = (int64_t)strtoll(v, NULL, 10);
    return 0;
}

int jwt_json_get_string(const char* json, const char* name, char** out) {
    if (!json || !name || !out) return -1;
    return json_get_str(json, name, out);
}
int jwt_json_get_int(const char* json, const char* name, int64_t* out) {
    if (!json || !name || !out) return -1;
    return json_get_int(json, name, out);
}

/* --- hash y comparación timing-safe ----------------------------------- */

char* jwt_core_hash_hex(const char* data) {
    if (!data) return NULL;
    uint8_t digest[GOST_HASH_256 / 8];
    if (gost_hash_text(data, strlen(data), GOST_HASH_256, digest) != GOST_OK)
        return NULL;
    char* out = (char*)malloc(sizeof(digest) * 2 + 1);
    if (!out) return NULL;
    hex_encode(digest, sizeof(digest), out);
    return out;
}

bool jwt_safe_compare(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t la = strlen(a), lb = strlen(b);
    if (la != lb) return false;
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < la; i++)
        diff |= (unsigned char)a[i] ^ (unsigned char)b[i];
    return diff == 0;
}

/* sign = hash(secret + A + B); A = id o "rand", B = tail (puede ser NULL) */
static char* payload_sign(const jwt_core_t* core, const char* A,
                          const char* B) {
    sbuf_t sb = {0};
    if (sbuf_append_str(&sb, core->secret) ||
        sbuf_append_str(&sb, A ? A : "") ||
        (B && sbuf_append_str(&sb, B))) {
        free(sb.data); return NULL;
    }
    char* hex = jwt_core_hash_hex(sb.data);
    free(sb.data);
    return hex;
}

/* --- ciclo de vida ---------------------------------------------------- */

jwt_core_t* jwt_core_new(const char* secret_key, const char* x_vector_hex) {
    if (!secret_key || !x_vector_hex || !*x_vector_hex) return NULL;

    uint32_t key[GOST_CIPHER_KEY_WORDS] = {0};
    {
        const char* p = x_vector_hex;
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

    jwt_core_t* core = (jwt_core_t*)calloc(1, sizeof(*core));
    if (!core) return NULL;
    core->secret = strdup(secret_key);
    if (!core->secret) { free(core); return NULL; }
    memcpy(core->cipher_key, key, sizeof(key));
    for (int i = 0; i < 8 && secret_key[i]; i++)
        ((uint8_t*)core->cipher_iv)[i] = (uint8_t)secret_key[i];
    return core;
}

void jwt_core_free(jwt_core_t* core) {
    if (!core) return;
    free(core->secret);
    free(core);
}

/* --- cifrado/descifrado JSON ----------------------------------------- */

static int sbuf_append_user(sbuf_t* sb, const jwt_user_t* u) {
    char id[24];
    snprintf(id, sizeof id, "%" PRId64, u->usu_id);
    if (sbuf_append_str(sb, "{\"usu_id\":") ||
        sbuf_append_str(sb, id) ||
        sbuf_append_str(sb, ",\"usu_nombre\":") ||
        sbuf_append_json_str(sb, u->usu_nombre) ||
        sbuf_append_str(sb, ",\"usu_correo\":") ||
        sbuf_append_json_str(sb, u->usu_correo) ||
        sbuf_append_str(sb, ",\"ip\":") ||
        sbuf_append_json_str(sb, u->ip) ||
        sbuf_append_str(sb, ",\"usu_salt\":") ||
        sbuf_append_json_str(sb, u->usu_salt) ||
        sbuf_append_str(sb, "}"))
        return -1;
    return 0;
}

static char* encrypt_json_b64(const jwt_core_t* core, const sbuf_t* json) {
    size_t plain_bytes = utf8_to_utf16le(json->data, strlen(json->data),
                                        NULL, 0);
    uint8_t* buf = (uint8_t*)malloc(plain_bytes + 1);
    if (!buf) return NULL;
    utf8_to_utf16le(json->data, strlen(json->data), buf, plain_bytes + 1);

    gost_cipher_t cipher;
    if (gost_cipher_init(&cipher, core->cipher_key,
                          GOST_SBOX_TC26_Z) != GOST_OK ||
        gost_cipher_cfb_encrypt(&cipher, core->cipher_iv, buf,
                                plain_bytes) != GOST_OK) {
        free(buf); return NULL;
    }
    char* token = b64_encode(buf, plain_bytes);
    free(buf);
    return token;
}

static char* decrypt_token(const jwt_core_t* core, const char* token,
                           size_t* out_len) {
    uint8_t* ct = b64_decode(token, out_len);
    if (!ct) return NULL;
    gost_cipher_t cipher;
    if (gost_cipher_init(&cipher, core->cipher_key,
                          GOST_SBOX_TC26_Z) != GOST_OK ||
        gost_cipher_cfb_decrypt(&cipher, core->cipher_iv, ct,
                                *out_len) != GOST_OK) {
        free(ct); return NULL;
    }
    char* json = utf16le_to_utf8(ct, *out_len);
    free(ct);
    return json;
}

/* --- writers --------------------------------------------------------- */

/* Access: payload {id, init, exp, session_type, user{...}, sign}.
 * sign = hash(secret + id + sign_tail); sign_tail suele ser user->ip (web)
 * o "mobile" (móvil). exp lo calcula el llamador (cfg.access_token_expiration
 * * 60000 o lo que aplique). */
char* jwt_core_write_access(jwt_core_t* core, const jwt_user_t* user,
                           int64_t session_type, int64_t exp_ms,
                           const char* sign_tail) {
    if (!core || !user) return NULL;
    int64_t now = now_ms();
    char id[24], init[24], exp[24], st[24];
    snprintf(id,   sizeof id,   "%" PRId64, user->usu_id);
    snprintf(init, sizeof init, "%" PRId64, now);
    snprintf(exp,  sizeof exp,  "%" PRId64, exp_ms ? exp_ms : now);
    snprintf(st,   sizeof st,   "%" PRId64, session_type);

    char* sign = payload_sign(core, id, sign_tail);
    if (!sign) return NULL;

    sbuf_t sb = {0};
    int ok = sbuf_append_str(&sb, "{\"id\":") == 0 &&
             sbuf_append_str(&sb, id) == 0 &&
             sbuf_append_str(&sb, ",\"init\":") == 0 &&
             sbuf_append_str(&sb, init) == 0 &&
             sbuf_append_str(&sb, ",\"exp\":") == 0 &&
             sbuf_append_str(&sb, exp) == 0 &&
             sbuf_append_str(&sb, ",\"session_type\":") == 0 &&
             sbuf_append_str(&sb, st) == 0 &&
             sbuf_append_str(&sb, ",\"user\":") == 0 &&
             sbuf_append_user(&sb, user) == 0 &&
             sbuf_append_str(&sb, ",\"sign\":") == 0 &&
             sbuf_append_json_str(&sb, sign) == 0 &&
             sbuf_append_str(&sb, "}") == 0;
    free(sign);
    if (!ok) { free(sb.data); return NULL; }
    char* token = encrypt_json_b64(core, &sb);
    free(sb.data);
    return token;
}

/* Refresh: mismo payload que access pero sin init y con "type":"refresh".
 * exp lo da el llamador (ej. refresh_days * 86400000). */
char* jwt_core_write_refresh(jwt_core_t* core, const jwt_user_t* user,
                            int64_t session_type, int64_t exp_ms,
                            const char* sign_tail) {
    if (!core || !user) return NULL;
    char id[24], exp[24], st[24];
    snprintf(id, sizeof id, "%" PRId64, user->usu_id);
    snprintf(exp, sizeof exp, "%" PRId64, exp_ms);
    snprintf(st,  sizeof st,  "%" PRId64, session_type);

    char* sign = payload_sign(core, id, sign_tail);
    if (!sign) return NULL;

    sbuf_t sb = {0};
    int ok = sbuf_append_str(&sb, "{\"id\":") == 0 &&
             sbuf_append_str(&sb, id) == 0 &&
             sbuf_append_str(&sb, ",\"exp\":") == 0 &&
             sbuf_append_str(&sb, exp) == 0 &&
             sbuf_append_str(&sb, ",\"type\":\"refresh\",\"session_type\":") == 0 &&
             sbuf_append_str(&sb, st) == 0 &&
             sbuf_append_str(&sb, ",\"user\":") == 0 &&
             sbuf_append_user(&sb, user) == 0 &&
             sbuf_append_str(&sb, ",\"sign\":") == 0 &&
             sbuf_append_json_str(&sb, sign) == 0 &&
             sbuf_append_str(&sb, "}") == 0;
    free(sign);
    if (!ok) { free(sb.data); return NULL; }
    char* token = encrypt_json_b64(core, &sb);
    free(sb.data);
    return token;
}

/* CSRF: rand 32 B hex + expiración. sign = hash(secret + rand + exp + sign_suffix).
 * El type (NULL o "refresh_csrf") se incluye si se pasa. */
char* jwt_core_write_csrf(jwt_core_t* core, int64_t exp_ms,
                         const char* type_or_null,
                         const char* sign_suffix) {
    if (!core) return NULL;
    uint8_t rb[JWT_CORE_RANDOM_HEX / 2];
    if (gost_rng_system(NULL, rb, sizeof rb) != GOST_OK) return NULL;
    char rand_hex[JWT_CORE_RANDOM_HEX + 1];
    hex_encode(rb, sizeof rb, rand_hex);

    int64_t exp = exp_ms ? exp_ms : now_ms();
    char exp_str[24];
    snprintf(exp_str, sizeof exp_str, "%" PRId64, exp);

    /* sign = hash(secret + rand + exp + sign_suffix) */
    sbuf_t sb = {0};
    if (sbuf_append_str(&sb, core->secret) ||
        sbuf_append_str(&sb, rand_hex) ||
        sbuf_append_str(&sb, exp_str) ||
        (sign_suffix && sbuf_append_str(&sb, sign_suffix))) {
        free(sb.data); return NULL;
    }
    char* sign = jwt_core_hash_hex(sb.data);
    free(sb.data);
    if (!sign) return NULL;

    sbuf_t js = {0};
    int ok = sbuf_append_str(&js, "{\"exp\":") == 0 &&
             sbuf_append_str(&js, exp_str) == 0 &&
             sbuf_append_str(&js, ",\"rand\":") == 0 &&
             sbuf_append_json_str(&js, rand_hex) == 0 &&
             (type_or_null ? (sbuf_append_str(&js, ",\"type\":") == 0 &&
                              sbuf_append_json_str(&js, type_or_null) == 0)
                            : 1) &&
             sbuf_append_str(&js, ",\"sign\":") == 0 &&
             sbuf_append_json_str(&js, sign) == 0 &&
             sbuf_append_str(&js, "}") == 0;
    free(sign);
    if (!ok) { free(js.data); return NULL; }
    char* token = encrypt_json_b64(core, &js);
    free(js.data);
    return token;
}

/* --- decode + verify ------------------------------------------------ */

static void user_free_fields(jwt_user_t* u) {
    free(u->usu_nombre);
    free(u->usu_correo);
    free(u->ip);
    free(u->usu_salt);
    memset(u, 0, sizeof(*u));
}

jwt_core_payload_t* jwt_core_decode(jwt_core_t* core, const char* token) {
    if (!core || !token) return NULL;
    size_t ct_len = 0;
    char* json = decrypt_token(core, token, &ct_len);
    if (!json) return NULL;
    /* Verificación mínima de expiración. */
    int64_t today = now_ms();

    jwt_core_payload_t* p = (jwt_core_payload_t*)calloc(1, sizeof(*p));
    if (!p) { free(json); return NULL; }

    /* Determinar kind: si hay "init" y "user" -> ACCESS/REFRESH; si hay "rand" -> CSRF. */
    int has_init = json_member(json, "init") != NULL;
    int has_rand = json_member(json, "rand") != NULL;
    int has_user = json_member(json, "user") != NULL;
    if (has_rand && !has_user) p->kind = JWT_CORE_KIND_CSRF;
    else if (has_user)        p->kind = has_init ? JWT_CORE_KIND_ACCESS
                                                  : JWT_CORE_KIND_REFRESH;
    else { free(p); free(json); return NULL; }

    if (json_get_int(json, "exp", &p->exp) ||
        (p->exp > 0 && p->exp < today)) {
        free(p); free(json); return NULL;
    }

    if (p->kind != JWT_CORE_KIND_CSRF) {
        /* id y session_type son obligatorios; init (access) y type
         * (refresh) son opcionales según el tipo. */
        if (json_get_int(json, "id", &p->id) ||
            json_get_int(json, "session_type", &p->session_type)) {
            free(p); free(json); return NULL;
        }
        json_get_int(json, "init", &p->init);
        json_get_str(json, "type", &p->type);
        const char* u = json_member(json, "user");
        if (!u) { free(p); free(json); return NULL; }
        if (json_get_int(u, "usu_id", &p->user.usu_id) ||
            json_get_str(u, "usu_nombre", &p->user.usu_nombre) ||
            json_get_str(u, "usu_correo", &p->user.usu_correo) ||
            json_get_str(u, "ip",            &p->user.ip) ||
            json_get_str(u, "usu_salt",      &p->user.usu_salt)) {
            user_free_fields(&p->user);
            free(p); free(json); return NULL;
        }
    } else {
        if (json_get_str(json, "rand", &p->rand)) {
            free(p); free(json); return NULL;
        }
        json_get_str(json, "type", &p->type);
    }
    if (json_get_str(json, "sign", &p->sign)) {
        jwt_core_payload_free(p);
        free(json); return NULL;
    }

    free(json);
    return p;
}

bool jwt_core_verify_sign(jwt_core_t* core, const jwt_core_payload_t* p,
                          const char* sign_tail) {
    if (!core || !p || !p->sign) return false;
    char* expected = NULL;
    if (p->kind == JWT_CORE_KIND_CSRF) {
        /* sign = hash(secret + rand + exp + sign_tail) */
        char exp_str[24];
        snprintf(exp_str, sizeof exp_str, "%" PRId64, p->exp);
        sbuf_t sb = {0};
        if (sbuf_append_str(&sb, core->secret) ||
            sbuf_append_str(&sb, p->rand) ||
            sbuf_append_str(&sb, exp_str) ||
            (sign_tail && sbuf_append_str(&sb, sign_tail))) {
            free(sb.data); return false;
        }
        expected = jwt_core_hash_hex(sb.data);
        free(sb.data);
    } else {
        char id[24];
        snprintf(id, sizeof id, "%" PRId64, p->id);
        expected = payload_sign(core, id, sign_tail);
    }
    if (!expected) return false;
    bool ok = jwt_safe_compare(expected, p->sign);
    free(expected);
    return ok;
}

bool jwt_core_verify_csrf(jwt_core_t* core, const char* token,
                          const char* sign_suffix) {
    if (!core || !token) return false;
    jwt_core_payload_t* p = jwt_core_decode(core, token);
    if (!p || p->kind != JWT_CORE_KIND_CSRF) {
        jwt_core_payload_free(p);
        return false;
    }
    bool ok = jwt_core_verify_sign(core, p, sign_suffix);
    jwt_core_payload_free(p);
    return ok;
}

void jwt_core_payload_free(jwt_core_payload_t* p) {
    if (!p) return;
    user_free_fields(&p->user);
    free(p->type);
    free(p->rand);
    free(p->sign);
    free(p);
}

/* --- generación/verificación de contraseñas --------------------------- */

char* jwt_core_generate_password(jwt_core_t* core, const char* password) {
    if (!core || !password) return NULL;
    while (*password == ' ' || *password == '\t') password++;
    size_t len = strlen(password);
    char* pw = strdup(password);
    if (!pw) return NULL;
    while (len > 0 && (pw[len - 1] == ' ' || pw[len - 1] == '\t'))
        pw[--len] = '\0';

    size_t plain_bytes = utf8_to_utf16le(pw, len, NULL, 0);
    uint8_t* buf = (uint8_t*)malloc(plain_bytes + 1);
    if (!buf) { free(pw); return NULL; }
    utf8_to_utf16le(pw, len, buf, plain_bytes + 1);

    gost_cipher_t cipher;
    if (gost_cipher_init(&cipher, core->cipher_key,
                          GOST_SBOX_TC26_Z) != GOST_OK ||
        gost_cipher_cfb_encrypt(&cipher, core->cipher_iv, buf,
                                plain_bytes) != GOST_OK) {
        free(buf); free(pw); return NULL;
    }
    char* token = b64_encode(buf, plain_bytes);
    free(buf);
    free(pw);
    return token;
}

bool jwt_core_verify_password(const char* password, const char* stored_hash) {
    if (!password || !stored_hash) return false;
    char* computed = jwt_core_hash_hex(password);
    if (!computed) return false;
    bool ok = strlen(computed) == strlen(stored_hash) &&
              jwt_safe_compare(computed, stored_hash);
    free(computed);
    return ok;
}
