#define _POSIX_C_SOURCE 200809L

#include "fcm.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <curl/curl.h>

#include "configuration.h"
#include "integrations/telegram/telegram_json.h"

#define FCM_MAX_RESPONSE (1024u * 1024u)
#define FCM_SCOPE "https://www.googleapis.com/auth/firebase.messaging"

typedef struct {
    char*  data;
    size_t len;
    size_t cap;
} buf_t;

static char g_client_email[256];
static char g_private_key[8192];
static char g_project_id[128];
static char g_token_uri[256];
static char g_access_token[4096];
static time_t g_token_expiry = 0;
static int g_enabled = 0;
static int g_dry_run = 0;
static int g_curl = 0;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int cfg_bool(const char* key, int def) {
    const char* v = cfg_getenv(key);
    if (!v || !*v) return def;
    return !(strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 ||
             strcasecmp(v, "no") == 0);
}

static void copy_str(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int buf_reserve(buf_t* b, size_t extra) {
    if (!b) return -1;
    if (b->len + extra + 1 <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 512;
    while (cap < b->len + extra + 1) cap *= 2;
    char* p = (char*)realloc(b->data, cap);
    if (!p) return -1;
    b->data = p;
    b->cap = cap;
    return 0;
}

static int buf_puts(buf_t* b, const char* s) {
    if (!s) s = "";
    size_t n = strlen(s);
    if (buf_reserve(b, n) != 0) return -1;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int buf_printf(buf_t* b, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char tmp[2048];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return -1;
    if ((size_t)n < sizeof(tmp)) return buf_puts(b, tmp);
    char* big = (char*)malloc((size_t)n + 1);
    if (!big) return -1;
    va_start(ap, fmt);
    vsnprintf(big, (size_t)n + 1, fmt, ap);
    va_end(ap);
    int rc = buf_puts(b, big);
    free(big);
    return rc;
}

static void buf_free(buf_t* b) {
    if (!b) return;
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static size_t write_cb(void* ptr, size_t size, size_t nmemb, void* ud) {
    buf_t* b = (buf_t*)ud;
    size_t n = size * nmemb;
    if (!b || n > FCM_MAX_RESPONSE || b->len + n > FCM_MAX_RESPONSE)
        return 0;
    if (buf_reserve(b, n) != 0) return 0;
    memcpy(b->data + b->len, ptr, n);
    b->len += n;
    b->data[b->len] = '\0';
    return n;
}

static char* http_request(const char* url, const char* body,
                          const char* content_type, const char* bearer,
                          long timeout, long* status_out) {
    if (!url || !body) return NULL;
    CURL* curl = curl_easy_init();
    if (!curl) return NULL;
    buf_t resp = {0};
    struct curl_slist* headers = NULL;
    char ctype[128];
    snprintf(ctype, sizeof(ctype), "Content-Type: %s", content_type);
    headers = curl_slist_append(headers, ctype);
    headers = curl_slist_append(headers, "Accept: application/json");
    if (bearer && *bearer) {
        char auth[4200];
        snprintf(auth, sizeof(auth), "Authorization: Bearer %s", bearer);
        headers = curl_slist_append(headers, auth);
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout > 0 ? timeout : 20L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    if (status_out) *status_out = status;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        free(resp.data);
        return NULL;
    }
    if (!resp.data) resp.data = strdup("");
    return resp.data;
}

static char* b64url(const unsigned char* data, size_t len) {
    size_t cap = 4 * ((len + 2) / 3) + 1;
    char* b64 = (char*)malloc(cap);
    if (!b64) return NULL;
    int n = EVP_EncodeBlock((unsigned char*)b64, data, (int)len);
    if (n < 0) { free(b64); return NULL; }
    b64[n] = '\0';
    for (int i = 0; i < n; i++) {
        if (b64[i] == '+') b64[i] = '-';
        else if (b64[i] == '/') b64[i] = '_';
    }
    while (n > 0 && b64[n - 1] == '=') b64[--n] = '\0';
    return b64;
}

static int read_file(const char* path, char** out) {
    *out = NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long n = ftell(f);
    if (n <= 0 || n > 128 * 1024) { fclose(f); return -1; }
    rewind(f);
    char* b = (char*)malloc((size_t)n + 1);
    if (!b) { fclose(f); return -1; }
    size_t r = fread(b, 1, (size_t)n, f);
    fclose(f);
    b[r] = '\0';
    *out = b;
    return 0;
}

static int load_credentials(const char* path) {
    char* text = NULL;
    if (read_file(path, &text) != 0) return 0;
    tj_value_t* root = tj_parse(text, strlen(text));
    free(text);
    if (!root) return 0;
    const char* email = tj_str(tj_obj_get(root, "client_email"));
    const char* key = tj_str(tj_obj_get(root, "private_key"));
    const char* project = tj_str(tj_obj_get(root, "project_id"));
    const char* uri = tj_str(tj_obj_get(root, "token_uri"));
    copy_str(g_client_email, sizeof(g_client_email), email);
    copy_str(g_private_key, sizeof(g_private_key), key);
    copy_str(g_project_id, sizeof(g_project_id),
             cfg_getenv("FCM_PROJECT_ID") && *cfg_getenv("FCM_PROJECT_ID")
                 ? cfg_getenv("FCM_PROJECT_ID") : project);
    copy_str(g_token_uri, sizeof(g_token_uri),
             uri && *uri ? uri : "https://oauth2.googleapis.com/token");
    tj_free(root);
    return g_client_email[0] && g_private_key[0] && g_project_id[0];
}

static int make_jwt(char** out) {
    *out = NULL;
    buf_t header = {0};
    buf_t claims = {0};
    if (buf_puts(&header, "{\"alg\":\"RS256\",\"typ\":\"JWT\"}") != 0)
        goto fail;
    time_t now = time(NULL);
    if (buf_printf(&claims,
                   "{\"iss\":\"%s\",\"scope\":\"%s\",\"aud\":\"%s\","
                   "\"iat\":%lld,\"exp\":%lld}",
                   g_client_email, FCM_SCOPE, g_token_uri,
                   (long long)now, (long long)(now + 3600)) != 0)
        goto fail;

    char* h = b64url((const unsigned char*)header.data, header.len);
    char* c = b64url((const unsigned char*)claims.data, claims.len);
    if (!h || !c) { free(h); free(c); goto fail; }
    size_t signing_len = strlen(h) + 1 + strlen(c);
    char* signing = (char*)malloc(signing_len + 1);
    if (!signing) { free(h); free(c); goto fail; }
    snprintf(signing, signing_len + 1, "%s.%s", h, c);

    BIO* bio = BIO_new_mem_buf(g_private_key, -1);
    EVP_PKEY* pkey = bio ? PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL) : NULL;
    if (bio) BIO_free(bio);
    if (!pkey) { free(signing); free(h); free(c); goto fail; }

    EVP_MD_CTX* md = EVP_MD_CTX_new();
    unsigned char sig[1024];
    size_t siglen = sizeof(sig);
    int ok = md &&
             EVP_DigestSignInit(md, NULL, EVP_sha256(), NULL, pkey) == 1 &&
             EVP_DigestSignUpdate(md, signing, signing_len) == 1 &&
             EVP_DigestSignFinal(md, sig, &siglen) == 1;
    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    if (!ok) { free(signing); free(h); free(c); goto fail; }

    char* s = b64url(sig, siglen);
    if (!s) { free(signing); free(h); free(c); goto fail; }
    buf_t jwt = {0};
    if (buf_printf(&jwt, "%s.%s", signing, s) != 0) {
        buf_free(&jwt);
        free(s); free(signing); free(h); free(c);
        goto fail;
    }
    *out = jwt.data;
    free(s); free(signing); free(h); free(c);
    buf_free(&header); buf_free(&claims);
    return 1;
fail:
    buf_free(&header); buf_free(&claims);
    return 0;
}

static int refresh_token(void) {
    if (g_dry_run) return 1;
    char* jwt = NULL;
    if (!make_jwt(&jwt)) return 0;
    buf_t body = {0};
    buf_puts(&body, "grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=");
    buf_puts(&body, jwt);
    free(jwt);
    long status = 0;
    char* resp = http_request(g_token_uri, body.data,
                              "application/x-www-form-urlencoded",
                              NULL, 20, &status);
    buf_free(&body);
    if (!resp) return 0;
    int ok = 0;
    if (status >= 200 && status < 300) {
        tj_value_t* root = tj_parse(resp, strlen(resp));
        if (root) {
            const char* at = tj_str(tj_obj_get(root, "access_token"));
            int64_t exp = tj_int(tj_obj_get(root, "expires_in"));
            if (at && *at) {
                copy_str(g_access_token, sizeof(g_access_token), at);
                g_token_expiry = time(NULL) + (exp > 0 ? exp : 3600);
                ok = 1;
            }
            tj_free(root);
        }
    }
    free(resp);
    return ok;
}

static const char* get_token(void) {
    pthread_mutex_lock(&g_lock);
    if (g_dry_run) {
        pthread_mutex_unlock(&g_lock);
        return "dry-run";
    }
    if (g_access_token[0] && g_token_expiry > time(NULL) + 300) {
        pthread_mutex_unlock(&g_lock);
        return g_access_token;
    }
    int ok = refresh_token();
    pthread_mutex_unlock(&g_lock);
    return ok ? g_access_token : NULL;
}

int fcm_init(void) {
    const char* enabled = cfg_getenv("FCM_ENABLED");
    if (!enabled || !*enabled ||
        (strcmp(enabled, "0") == 0 || strcasecmp(enabled, "false") == 0)) {
        g_enabled = 0;
        return 0;
    }
    g_dry_run = cfg_bool("FCM_DRY_RUN", 0);
    if (!g_dry_run) {
        const char* path = cfg_getenv("FCM_CREDENTIALS");
        if (!path || !*path) path = "/etc/cws/fcm-service-account.json";
        if (!load_credentials(path)) {
            fprintf(stderr, "fcm: no se pudieron cargar credenciales %s\n", path);
            g_enabled = 0;
            return 0;
        }
    } else {
        copy_str(g_project_id, sizeof(g_project_id),
                 cfg_getenv("FCM_PROJECT_ID") ? cfg_getenv("FCM_PROJECT_ID")
                                              : "dry-run");
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0) {
        g_enabled = 0;
        return 0;
    }
    g_curl = 1;
    g_enabled = 1;
    fprintf(stderr, "fcm: activo project=%s dry_run=%d\n",
            g_project_id, g_dry_run);
    return 1;
}

void fcm_shutdown(void) {
    if (g_curl) {
        curl_global_cleanup();
        g_curl = 0;
    }
    g_enabled = 0;
    g_access_token[0] = '\0';
    g_token_expiry = 0;
}

int fcm_enabled(void) {
    return g_enabled;
}

int fcm_send(const char* token, const char* payload_json,
             const char* priority, int ttl, const char* collapse,
             char* err, size_t errcap) {
    if (err && errcap) err[0] = '\0';
    if (!g_enabled || !token || !*token || !payload_json ||
        payload_json[0] != '{') {
        if (err && errcap) snprintf(err, errcap, "payload/token inválido");
        return FCM_ERROR;
    }
    if (g_dry_run) {
        fprintf(stderr, "fcm: dry-run token=%.8s type_ttl=%d\n", token, ttl);
        return FCM_SENT;
    }
    const char* bearer = get_token();
    if (!bearer) {
        if (err && errcap) snprintf(err, errcap, "sin access_token");
        return FCM_ERROR;
    }

    buf_t body = {0};
    buf_puts(&body, "{\"message\":{\"token\":\"");
    /* token FCM: solo caracteres URL-safe en la práctica; escapar comillas. */
    for (const char* p = token; *p; p++) {
        if (*p == '"' || *p == '\\') buf_puts(&body, "\\");
        char one[2] = { *p, 0 };
        buf_puts(&body, one);
    }
    buf_puts(&body, "\",\"data\":");
    buf_puts(&body, payload_json);
    buf_puts(&body, ",\"android\":{\"priority\":\"");
    buf_puts(&body, (priority && strcmp(priority, "high") == 0) ? "high"
                                                                : "normal");
    buf_puts(&body, "\",\"ttl\":\"");
    char ttl_s[32];
    snprintf(ttl_s, sizeof(ttl_s), "%ds", ttl > 0 ? ttl : 60);
    buf_puts(&body, ttl_s);
    buf_puts(&body, "\"");
    if (collapse && *collapse) {
        buf_puts(&body, ",\"collapse_key\":\"");
        for (const char* p = collapse; *p; p++) {
            if (*p == '"' || *p == '\\') buf_puts(&body, "\\");
            char one[2] = { *p, 0 };
            buf_puts(&body, one);
        }
        buf_puts(&body, "\"");
    }
    buf_puts(&body, "}}}");
    if (!body.data) {
        if (err && errcap) snprintf(err, errcap, "sin memoria");
        return FCM_ERROR;
    }

    char url[512];
    snprintf(url, sizeof(url),
             "https://fcm.googleapis.com/v1/projects/%s/messages:send",
             g_project_id);
    long status = 0;
    char* resp = http_request(url, body.data, "application/json",
                              bearer, 20, &status);
    buf_free(&body);
    if (!resp) {
        if (err && errcap) snprintf(err, errcap, "HTTP FCM");
        return FCM_ERROR;
    }
    int rc = FCM_ERROR;
    if (status >= 200 && status < 300) {
        rc = FCM_SENT;
    } else {
        int invalid = strstr(resp, "UNREGISTERED") != NULL ||
                      strstr(resp, "INVALID_ARGUMENT") != NULL ||
                      status == 404;
        if (err && errcap) {
            const char* p = strstr(resp, "\"message\"");
            snprintf(err, errcap, "FCM HTTP %ld%s", status, p ? p : "");
        }
        rc = invalid ? FCM_INVALID_TOKEN : FCM_ERROR;
    }
    free(resp);
    return rc;
}
