#define _POSIX_C_SOURCE 200809L

#include "telegram_json.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct tj_value {
    tj_type_t type;
    char*     str;
    int64_t   num;
    int       boolean;
    char**    keys;
    tj_value_t** items;
    size_t    count;
    size_t    cap;
};

typedef struct {
    const char* p;
    const char* end;
    int         depth;
} parser_t;

#define TJ_MAX_DEPTH 64

static void skip_ws(parser_t* ps) {
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' ||
                               *ps->p == '\r' || *ps->p == '\n'))
        ps->p++;
}

static void utf8_put(char** out, size_t* len, size_t* cap, uint32_t cp) {
    if (cp <= 0x7F) {
        if (*len + 1 >= *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *out = (char*)realloc(*out, *cap);
        }
        (*out)[(*len)++] = (char)cp;
    } else if (cp <= 0x7FF) {
        if (*len + 2 >= *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *out = (char*)realloc(*out, *cap);
        }
        (*out)[(*len)++] = (char)(0xC0 | (cp >> 6));
        (*out)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        if (*len + 3 >= *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *out = (char*)realloc(*out, *cap);
        }
        (*out)[(*len)++] = (char)(0xE0 | (cp >> 12));
        (*out)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*out)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        if (*len + 4 >= *cap) {
            *cap = *cap ? *cap * 2 : 64;
            *out = (char*)realloc(*out, *cap);
        }
        (*out)[(*len)++] = (char)(0xF0 | (cp >> 18));
        (*out)[(*len)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        (*out)[(*len)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        (*out)[(*len)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static int hex4(const char* p, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (uint32_t)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

/* Parses a JSON string starting at '"'. Returns malloc'd UTF-8. */
static char* parse_string_raw(parser_t* ps) {
    if (ps->p >= ps->end || *ps->p != '"') return NULL;
    ps->p++;
    char* out = NULL;
    size_t len = 0, cap = 0;
    while (ps->p < ps->end) {
        unsigned char c = (unsigned char)*ps->p++;
        if (c == '"') {
            if (!out) out = (char*)calloc(1, 1);
            else out[len] = '\0';
            return out ? out : NULL;
        }
        if (c == '\\') {
            if (ps->p >= ps->end) break;
            char e = *ps->p++;
            switch (e) {
                case '"':  c = '"';  break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u': {
                    uint32_t cp = 0;
                    if (ps->end - ps->p < 4 || hex4(ps->p, &cp) != 0)
                        goto fail;
                    ps->p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF &&
                        ps->end - ps->p >= 6 && ps->p[0] == '\\' &&
                        ps->p[1] == 'u') {
                        uint32_t lo = 0;
                        if (hex4(ps->p + 2, &lo) == 0 &&
                            lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) +
                                 (lo - 0xDC00);
                            ps->p += 6;
                        }
                    }
                    utf8_put(&out, &len, &cap, cp);
                    continue;
                }
                default:
                    c = (unsigned char)e;
                    break;
            }
        }
        if (len + 2 >= cap) {
            cap = cap ? cap * 2 : 64;
            char* n = (char*)realloc(out, cap);
            if (!n) goto fail;
            out = n;
        }
        out[len++] = (char)c;
    }
fail:
    free(out);
    return NULL;
}

static tj_value_t* value_new(tj_type_t type) {
    tj_value_t* v = (tj_value_t*)calloc(1, sizeof(*v));
    if (v) v->type = type;
    return v;
}

static int value_append(tj_value_t* v, char* key, tj_value_t* item) {
    if (v->count == v->cap) {
        size_t ncap = v->cap ? v->cap * 2 : 8;
        char** nk = (char**)realloc(v->keys, ncap * sizeof(*nk));
        if (!nk) return -1;
        v->keys = nk;
        tj_value_t** ni =
            (tj_value_t**)realloc(v->items, ncap * sizeof(*ni));
        if (!ni) return -1;
        v->items = ni;
        v->cap = ncap;
    }
    v->keys[v->count] = key;
    v->items[v->count] = item;
    v->count++;
    return 0;
}

static tj_value_t* parse_value(parser_t* ps);

static tj_value_t* parse_object(parser_t* ps) {
    if (ps->depth++ > TJ_MAX_DEPTH) { ps->depth--; return NULL; }
    if (ps->p >= ps->end || *ps->p != '{') { ps->depth--; return NULL; }
    ps->p++;
    tj_value_t* v = value_new(TJ_OBJ);
    if (!v) { ps->depth--; return NULL; }
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        ps->depth--;
        return v;
    }
    for (;;) {
        skip_ws(ps);
        char* key = parse_string_raw(ps);
        if (!key) goto fail;
        skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') { free(key); goto fail; }
        ps->p++;
        tj_value_t* item = parse_value(ps);
        if (!item) { free(key); goto fail; }
        if (value_append(v, key, item) != 0) {
            free(key);
            tj_free(item);
            goto fail;
        }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            ps->depth--;
            return v;
        }
        goto fail;
    }
fail:
    tj_free(v);
    ps->depth--;
    return NULL;
}

static tj_value_t* parse_array(parser_t* ps) {
    if (ps->depth++ > TJ_MAX_DEPTH) { ps->depth--; return NULL; }
    if (ps->p >= ps->end || *ps->p != '[') { ps->depth--; return NULL; }
    ps->p++;
    tj_value_t* v = value_new(TJ_ARR);
    if (!v) { ps->depth--; return NULL; }
    skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        ps->depth--;
        return v;
    }
    for (;;) {
        tj_value_t* item = parse_value(ps);
        if (!item) goto fail;
        if (value_append(v, NULL, item) != 0) {
            tj_free(item);
            goto fail;
        }
        skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') { ps->p++; continue; }
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            ps->depth--;
            return v;
        }
        goto fail;
    }
fail:
    tj_free(v);
    ps->depth--;
    return NULL;
}

static tj_value_t* parse_number(parser_t* ps) {
    const char* start = ps->p;
    if (ps->p < ps->end && (*ps->p == '-' || *ps->p == '+')) ps->p++;
    int digits = 0;
    while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) {
        ps->p++;
        digits++;
    }
    if (!digits) return NULL;
    if (ps->p < ps->end && *ps->p == '.') {
        ps->p++;
        while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) ps->p++;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
        ps->p++;
        if (ps->p < ps->end && (*ps->p == '-' || *ps->p == '+')) ps->p++;
        while (ps->p < ps->end && isdigit((unsigned char)*ps->p)) ps->p++;
    }
    const char* save = ps->p;
    ps->p = start;
    int64_t n = 0;
    int neg = 0;
    if (ps->p < ps->end && (*ps->p == '-' || *ps->p == '+')) {
        neg = (*ps->p == '-');
        ps->p++;
    }
    while (ps->p < ps->end && isdigit((unsigned char)*ps->p))
        n = n * 10 + (*ps->p++ - '0');
    ps->p = save;
    tj_value_t* v = value_new(TJ_NUM);
    if (v) v->num = neg ? -n : n;
    return v;
}

static tj_value_t* parse_value(parser_t* ps) {
    skip_ws(ps);
    if (ps->p >= ps->end) return NULL;
    switch (*ps->p) {
        case '{': return parse_object(ps);
        case '[': return parse_array(ps);
        case '"': {
            char* s = parse_string_raw(ps);
            if (!s) return NULL;
            tj_value_t* v = value_new(TJ_STR);
            if (!v) { free(s); return NULL; }
            v->str = s;
            return v;
        }
        case 't':
            if (ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
                ps->p += 4;
                tj_value_t* v = value_new(TJ_BOOL);
                if (v) v->boolean = 1;
                return v;
            }
            return NULL;
        case 'f':
            if (ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
                ps->p += 5;
                tj_value_t* v = value_new(TJ_BOOL);
                if (v) v->boolean = 0;
                return v;
            }
            return NULL;
        case 'n':
            if (ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
                ps->p += 4;
                return value_new(TJ_NULL);
            }
            return NULL;
        default:
            if (*ps->p == '-' || *ps->p == '+' ||
                isdigit((unsigned char)*ps->p))
                return parse_number(ps);
            return NULL;
    }
}

tj_value_t* tj_parse(const char* text, size_t len) {
    if (!text) return NULL;
    parser_t ps = { text, text + len, 0 };
    tj_value_t* v = parse_value(&ps);
    if (!v) return NULL;
    skip_ws(&ps);
    if (ps.p != ps.end) {
        tj_free(v);
        return NULL;
    }
    return v;
}

void tj_free(tj_value_t* v) {
    if (!v) return;
    free(v->str);
    for (size_t i = 0; i < v->count; i++) {
        free(v->keys ? v->keys[i] : NULL);
        tj_free(v->items ? v->items[i] : NULL);
    }
    free(v->keys);
    free(v->items);
    free(v);
}

tj_value_t* tj_obj_get(const tj_value_t* obj, const char* key) {
    if (!obj || obj->type != TJ_OBJ || !key) return NULL;
    for (size_t i = 0; i < obj->count; i++) {
        if (obj->keys && obj->keys[i] && strcmp(obj->keys[i], key) == 0)
            return obj->items[i];
    }
    return NULL;
}

tj_value_t* tj_arr_get(const tj_value_t* arr, size_t idx) {
    if (!arr || arr->type != TJ_ARR || idx >= arr->count) return NULL;
    return arr->items[idx];
}

size_t tj_arr_len(const tj_value_t* arr) {
    return (arr && arr->type == TJ_ARR) ? arr->count : 0;
}

const char* tj_str(const tj_value_t* v) {
    return (v && v->type == TJ_STR) ? v->str : NULL;
}
int64_t tj_int(const tj_value_t* v) {
    return (v && v->type == TJ_NUM) ? v->num : 0;
}
int tj_bool(const tj_value_t* v) {
    return (v && v->type == TJ_BOOL) ? v->boolean : 0;
}
tj_type_t tj_type(const tj_value_t* v) {
    return v ? v->type : TJ_NULL;
}
