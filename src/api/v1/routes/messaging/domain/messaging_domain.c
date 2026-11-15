/*
 * messaging_domain.c — acceso a datos del chat 1:1.
 *
 * Ejecuta procChatMensajesCons (historial) y procChatMensajesProc
 * (insertar, marcar leído) vía core/sql_eject.
 */

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "messaging_domain.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "configuration.h"

#define MESSAGING_DB "soda_stream"

/* procChatMensajesCons */
#define CONS_CHAT_MSG_HISTORY 1
/* procChatMensajesProc */
#define PROC_CHAT_MSG_INS  1
#define PROC_CHAT_MSG_READ 2
/* procChatStickersProc */
#define PROC_CHAT_STICKER_INS  1
#define PROC_CHAT_STICKER_SYNC 2
/* procChatStickersCons */
#define CONS_CHAT_STICKER_GET   1
#define CONS_CHAT_STICKER_CHECK 2

static sql_eject_t* g_sql = NULL;

#define STICKERS_DIR_MAX 1024

static char g_stickers_dir[STICKERS_DIR_MAX];
static int  g_stickers_dir_set = 0;

static void normalize_dir(const char* in, char* out, size_t cap) {
    if (!in) { out[0] = '\0'; return; }
    size_t len = strlen(in);
    while (len > 0 && (unsigned char)in[len - 1] <= ' ') len--;
    while (len > 0 && (unsigned char)*in <= ' ') { in++; len--; }
    if (len >= 2 && (in[0] == '\'' || in[0] == '"') && in[len - 1] == in[0]) {
        in++;
        len -= 2;
    }
    if (len >= cap) len = cap - 1;
    memcpy(out, in, len);
    out[len] = '\0';
}

/* mkdir -p (ignora EEXIST). 0 en éxito. */
static int mkdir_p(const char* path) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) { errno = ENAMETOOLONG; return -1; }
    memcpy(tmp, path, len + 1);
    for (char* p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { *p = '/'; return -1; }
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int dir_ready(const char* d, const char* which) {
    if (!d || !*d) return 0;
    if (mkdir_p(d) != 0) {
        cws_log_warn("stickers: %s=%s no se pudo crear: %s", which, d,
                     strerror(errno));
        return 0;
    }
    if (access(d, W_OK) != 0) {
        cws_log_warn("stickers: %s=%s sin permiso de escritura: %s", which, d,
                     strerror(errno));
        return 0;
    }
    return 1;
}

static void resolve_dir(const char* d, char* out, size_t cap) {
    char real[4096];
    if (realpath(d, real))
        snprintf(out, cap, "%s", real);
    else
        snprintf(out, cap, "%s", d);
}

/* Elige el primer directorio usable: STICKERS_DIR (absoluto) y, si falla,
 * STICKERS_DIR_RELATIVE; como último recurso public/stickers. */
static void select_stickers_dir(void) {
    char absv[STICKERS_DIR_MAX], relv[STICKERS_DIR_MAX];
    normalize_dir(cfg_getenv("STICKERS_DIR"), absv, sizeof(absv));
    normalize_dir(cfg_getenv("STICKERS_DIR_RELATIVE"), relv, sizeof(relv));

    g_stickers_dir[0] = '\0';
    g_stickers_dir_set = 0;

    if (absv[0] && dir_ready(absv, "STICKERS_DIR")) {
        resolve_dir(absv, g_stickers_dir, sizeof(g_stickers_dir));
        g_stickers_dir_set = 1;
        return;
    }
    if (relv[0] && dir_ready(relv, "STICKERS_DIR_RELATIVE")) {
        resolve_dir(relv, g_stickers_dir, sizeof(g_stickers_dir));
        g_stickers_dir_set = 1;
        return;
    }
    if (dir_ready("public/stickers", "public/stickers")) {
        resolve_dir("public/stickers", g_stickers_dir,
                    sizeof(g_stickers_dir));
        g_stickers_dir_set = 1;
    }
}

const char* messaging_domain_stickers_dir(void) {
    if (!g_stickers_dir_set) select_stickers_dir();
    return g_stickers_dir;
}

int messaging_domain_stickers_ready(void) {
    if (!g_stickers_dir_set) select_stickers_dir();
    if (g_stickers_dir[0] && access(g_stickers_dir, W_OK) == 0) return 1;
    /* Reintenta la selección por si cambió el entorno/montaje. */
    select_stickers_dir();
    return g_stickers_dir[0] != '\0';
}

void messaging_domain_init(void) {
    app_config_t* cfg = configuration_new();
    if (!cfg) return;
    sql_eject_t* se = sql_eject_new(cfg);
    configuration_free(cfg);
    if (!se) return;
    sql_eject_free(g_sql);
    g_sql = se;

    if (messaging_domain_stickers_ready())
        cws_log_info("stickers: dir %s listo",
                     messaging_domain_stickers_dir());
    else
        cws_log_error("stickers: sin STICKERS_DIR/STICKERS_DIR_RELATIVE usable");
}

void messaging_domain_shutdown(void) {
    sql_eject_free(g_sql);
    g_sql = NULL;
}

sql_result_t* messaging_domain_history(int64_t usu_id, int64_t usu_id_otro,
                                       int64_t msg_before, int limit) {
    if (!g_sql) return NULL;

    sql_param_t params[5];
    size_t n = 0;

    params[n].name = "tipoConsulta";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = CONS_CHAT_MSG_HISTORY;
    n++;

    params[n].name = "usu_id";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = usu_id;
    n++;

    params[n].name = "usu_id_otro";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = usu_id_otro;
    n++;

    params[n].name = "msg_before";
    params[n].type = msg_before > 0 ? SQL_PT_INT : SQL_PT_NULL;
    params[n].val.as_int = msg_before;
    n++;

    params[n].name = "limit";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = limit;
    n++;

    sql_result_t* out = (sql_result_t*)calloc(1, sizeof(*out));
    if (!out) return NULL;
    int rc = sql_eject_store(g_sql, "procChatMensajesCons", MESSAGING_DB,
                             params, n, out);
    if (rc != CWS_OK) {
        sql_result_free(out);
        return NULL;
    }
    return out;
}

sql_result_t* messaging_domain_insert(int64_t emisor, int64_t receptor,
                                      int tipo,
                                      const char* sticker_id,
                                      const char* texto, int64_t reply_to) {
    if (!g_sql) return NULL;

    sql_param_t params[7];
    size_t n = 0;

    params[n].name = "tipoRegistro";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = PROC_CHAT_MSG_INS;
    n++;

    params[n].name = "msg_usu_emisor";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = emisor;
    n++;

    params[n].name = "msg_usu_receptor";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = receptor;
    n++;

    params[n].name = "msg_texto";
    params[n].type = SQL_PT_STRING;
    params[n].val.as_string = texto ? texto : "";
    n++;

    params[n].name = "msg_id_respuesta";
    params[n].type = reply_to > 0 ? SQL_PT_INT : SQL_PT_NULL;
    params[n].val.as_int = reply_to;
    n++;

    params[n].name = "tipo";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = tipo;
    n++;

    params[n].name = "sticker_id";
    params[n].type = (sticker_id && *sticker_id) ? SQL_PT_STRING : SQL_PT_NULL;
    params[n].val.as_string = sticker_id;
    n++;

    sql_result_t* out = (sql_result_t*)calloc(1, sizeof(*out));
    if (!out) return NULL;
    int rc = sql_eject_store(g_sql, "procChatMensajesProc", MESSAGING_DB,
                             params, n, out);
    if (rc != CWS_OK) {
        sql_result_free(out);
        return NULL;
    }
    return out;
}

sql_result_t* messaging_domain_mark_read(int64_t lector, int64_t otro,
                                         int64_t hasta) {
    if (!g_sql) return NULL;

    sql_param_t params[4];
    size_t n = 0;

    params[n].name = "tipoRegistro";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = PROC_CHAT_MSG_READ;
    n++;

    params[n].name = "msg_usu_lector";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = lector;
    n++;

    params[n].name = "msg_usu_otro";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = otro;
    n++;

    params[n].name = "msg_hasta";
    params[n].type = hasta > 0 ? SQL_PT_INT : SQL_PT_NULL;
    params[n].val.as_int = hasta;
    n++;

    sql_result_t* out = (sql_result_t*)calloc(1, sizeof(*out));
    if (!out) return NULL;
    int rc = sql_eject_store(g_sql, "procChatMensajesProc", MESSAGING_DB,
                             params, n, out);
    if (rc != CWS_OK) {
        sql_result_free(out);
        return NULL;
    }
    return out;
}

sql_result_t* messaging_domain_sticker_insert(const char* sti_hash,
                                              const char* mime,
                                              int64_t bytes, int width,
                                              int height, int animated,
                                              int64_t created_by) {
    if (!g_sql || !sti_hash) return NULL;

    sql_param_t params[8];
    size_t n = 0;

    params[n].name = "tipoRegistro";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = PROC_CHAT_STICKER_INS;
    n++;

    params[n].name = "sti_hash";
    params[n].type = SQL_PT_STRING;
    params[n].val.as_string = sti_hash;
    n++;

    params[n].name = "sti_mime";
    params[n].type = SQL_PT_STRING;
    params[n].val.as_string = (mime && *mime) ? mime : "image/webp";
    n++;

    params[n].name = "sti_bytes";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = bytes;
    n++;

    params[n].name = "sti_width";
    params[n].type = width > 0 ? SQL_PT_INT : SQL_PT_NULL;
    params[n].val.as_int = width;
    n++;

    params[n].name = "sti_height";
    params[n].type = height > 0 ? SQL_PT_INT : SQL_PT_NULL;
    params[n].val.as_int = height;
    n++;

    params[n].name = "sti_animated";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = animated ? 1 : 0;
    n++;

    params[n].name = "sti_id_created_by";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = created_by;
    n++;

    sql_result_t* out = (sql_result_t*)calloc(1, sizeof(*out));
    if (!out) return NULL;
    int rc = sql_eject_store(g_sql, "procChatStickersProc", MESSAGING_DB,
                             params, n, out);
    if (rc != CWS_OK) {
        sql_result_free(out);
        return NULL;
    }
    return out;
}

sql_result_t* messaging_domain_sticker_get(const char* sti_hash) {
    if (!g_sql || !sti_hash) return NULL;

    sql_param_t params[2];
    params[0].name = "tipoConsulta";
    params[0].type = SQL_PT_INT;
    params[0].val.as_int = CONS_CHAT_STICKER_GET;
    params[1].name = "sti_hash";
    params[1].type = SQL_PT_STRING;
    params[1].val.as_string = sti_hash;

    sql_result_t* out = (sql_result_t*)calloc(1, sizeof(*out));
    if (!out) return NULL;
    int rc = sql_eject_store(g_sql, "procChatStickersCons", MESSAGING_DB,
                             params, 2, out);
    if (rc != CWS_OK) {
        sql_result_free(out);
        return NULL;
    }
    return out;
}

sql_result_t* messaging_domain_stickers_check(const char* ids_csv,
                                              int64_t contact_usu_id) {
    if (!g_sql) return NULL;

    sql_param_t params[3];
    params[0].name = "tipoConsulta";
    params[0].type = SQL_PT_INT;
    params[0].val.as_int = CONS_CHAT_STICKER_CHECK;
    params[1].name = "usu_id";
    params[1].type = contact_usu_id > 0 ? SQL_PT_INT : SQL_PT_NULL;
    params[1].val.as_int = contact_usu_id;
    params[2].name = "sti_ids";
    params[2].type = SQL_PT_STRING;
    params[2].val.as_string = ids_csv ? ids_csv : "";

    sql_result_t* out = (sql_result_t*)calloc(1, sizeof(*out));
    if (!out) return NULL;
    int rc = sql_eject_store(g_sql, "procChatStickersCons", MESSAGING_DB,
                             params, 3, out);
    if (rc != CWS_OK) {
        sql_result_free(out);
        return NULL;
    }
    return out;
}

sql_result_t* messaging_domain_stickers_sync(int64_t usu_id,
                                             const char* ids_csv) {
    if (!g_sql) return NULL;

    sql_param_t params[3];
    params[0].name = "tipoRegistro";
    params[0].type = SQL_PT_INT;
    params[0].val.as_int = PROC_CHAT_STICKER_SYNC;
    params[1].name = "usu_id";
    params[1].type = SQL_PT_INT;
    params[1].val.as_int = usu_id;
    params[2].name = "sti_ids";
    params[2].type = SQL_PT_STRING;
    params[2].val.as_string = ids_csv ? ids_csv : "";

    sql_result_t* out = (sql_result_t*)calloc(1, sizeof(*out));
    if (!out) return NULL;
    int rc = sql_eject_store(g_sql, "procChatStickersProc", MESSAGING_DB,
                             params, 3, out);
    if (rc != CWS_OK) {
        sql_result_free(out);
        return NULL;
    }
    return out;
}
