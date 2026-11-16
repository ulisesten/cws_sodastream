#define _POSIX_C_SOURCE 200809L

#include "telegram_domain.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "configuration.h"
#include "sql_eject.h"

#define TELEGRAM_DB "soda_stream"

#define PROC_TG_CODIGO   1
#define PROC_TG_VINCULAR 2
#define PROC_TG_UNLINK   3

#define CONS_TG_LINK_USUARIO 1
#define CONS_TG_LINK_CHAT    2
#define CONS_TG_USER_PUBLIC  4

#define PROC_INT_LINK   1
#define PROC_INT_UNLINK 2
#define CONS_INT_LOOKUP 1

static sql_eject_t* g_sql = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int run_sp(const char* sp, const sql_param_t* params, size_t n,
                  sql_result_t* out) {
    if (!g_sql || !out) return -1;
    pthread_mutex_lock(&g_lock);
    int rc = sql_eject_store(g_sql, sp, TELEGRAM_DB, params, n, out);
    pthread_mutex_unlock(&g_lock);
    return rc == CWS_OK ? 0 : -1;
}

static const sql_value_t* row_field(const sql_result_t* out, size_t row,
                                    const char* name) {
    if (!out || row >= out->nrows) return NULL;
    for (size_t c = 0; c < out->ncols; c++) {
        const char* cn = out->columns[c].name;
        if (cn && strcmp(cn, name) == 0)
            return &out->rows[row].cols[c];
    }
    return NULL;
}

static const char* row_str(const sql_result_t* out, size_t row,
                           const char* name) {
    const sql_value_t* v = row_field(out, row, name);
    if (!v || v->is_null || v->type != SQL_COL_STRING || !v->val.as_string)
        return NULL;
    return v->val.as_string;
}

static int64_t row_i64(const sql_result_t* out, size_t row, const char* name,
                       int64_t def) {
    const sql_value_t* v = row_field(out, row, name);
    if (!v || v->is_null || v->type != SQL_COL_INT) return def;
    return v->val.as_int;
}

static int row_ok(const sql_result_t* out) {
    const char* s = row_str(out, 0, "success");
    return s && (strcmp(s, "true") == 0 || strcmp(s, "1") == 0);
}

static void copy_str(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void telegram_domain_init(void) {
    app_config_t* cfg = configuration_new();
    if (!cfg) return;
    sql_eject_t* se = sql_eject_new(cfg);
    configuration_free(cfg);
    if (!se) return;
    sql_eject_free(g_sql);
    g_sql = se;
}

void telegram_domain_shutdown(void) {
    pthread_mutex_lock(&g_lock);
    sql_eject_free(g_sql);
    g_sql = NULL;
    pthread_mutex_unlock(&g_lock);
}

int telegram_domain_create_code(int64_t usu_id, const char* code, int ttl_min) {
    if (!g_sql || !code || !*code) return 0;

    sql_param_t p[4];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoProceso", SQL_PT_INT,
                            { .as_int = PROC_TG_CODIGO } };
    p[n++] = (sql_param_t){ "lic_id_usuario", SQL_PT_INT, { .as_int = usu_id } };
    p[n++] = (sql_param_t){ "lic_codigo", SQL_PT_STRING, { .as_string = code } };
    p[n++] = (sql_param_t){ "lic_ttl_min", SQL_PT_INT,
                            { .as_int = ttl_min > 0 ? ttl_min : 15 } };

    sql_result_t out;
    memset(&out, 0, sizeof(out));
    int rc = run_sp("procLlamadasIntegracionVinculosProc", p, n, &out);
    int ok = (rc == 0) && row_ok(&out);
    sql_result_free(&out);
    return ok;
}

int telegram_domain_consume_code(const char* code, int64_t chat_id,
                                 const char* username, const char* name,
                                 int64_t* usu_id_out) {
    if (!g_sql || !code || !*code) return 0;

    sql_param_t p[6];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoProceso", SQL_PT_INT,
                            { .as_int = PROC_TG_VINCULAR } };
    p[n++] = (sql_param_t){ "lic_codigo", SQL_PT_STRING, { .as_string = code } };
    p[n++] = (sql_param_t){ "liv_chat_id", SQL_PT_INT, { .as_int = chat_id } };
    p[n++] = (sql_param_t){ "liv_usuario_tg", username ? SQL_PT_STRING : SQL_PT_NULL,
                            { .as_string = username } };
    p[n++] = (sql_param_t){ "liv_nombre_tg", name ? SQL_PT_STRING : SQL_PT_NULL,
                            { .as_string = name } };

    sql_result_t out;
    memset(&out, 0, sizeof(out));
    int rc = run_sp("procLlamadasIntegracionVinculosProc", p, n, &out);
    int ok = (rc == 0) && row_ok(&out);
    if (ok && usu_id_out)
        *usu_id_out = row_i64(&out, 0, "usu_id", 0);
    sql_result_free(&out);
    return ok;
}

static int get_link(sql_param_t* p, size_t n, telegram_link_t* out) {
    sql_result_t outres;
    memset(&outres, 0, sizeof(outres));
    int rc = run_sp("procLlamadasIntegracionVinculosCons", p, n, &outres);
    int ok = 0;
    if (rc == 0 && outres.nrows > 0) {
        const char* s = row_str(&outres, 0, "success");
        if (!(s && strcmp(s, "false") == 0)) {
            if (out) {
                memset(out, 0, sizeof(*out));
                out->usu_id = row_i64(&outres, 0, "liv_id_usuario", 0);
                out->chat_id = row_i64(&outres, 0, "liv_chat_id", 0);
                copy_str(out->username, sizeof(out->username),
                         row_str(&outres, 0, "liv_usuario_tg"));
                copy_str(out->name, sizeof(out->name),
                         row_str(&outres, 0, "liv_nombre_tg"));
                out->active = (int)row_i64(&outres, 0, "liv_activo", 0);
            }
            ok = 1;
        }
    }
    sql_result_free(&outres);
    return ok;
}

int telegram_domain_get_link(int64_t usu_id, telegram_link_t* out) {
    sql_param_t p[2];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoConsulta", SQL_PT_INT,
                            { .as_int = CONS_TG_LINK_USUARIO } };
    p[n++] = (sql_param_t){ "lic_id_usuario", SQL_PT_INT,
                            { .as_int = usu_id } };
    return get_link(p, n, out);
}

int telegram_domain_get_link_by_chat(int64_t chat_id, telegram_link_t* out) {
    sql_param_t p[2];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoConsulta", SQL_PT_INT,
                            { .as_int = CONS_TG_LINK_CHAT } };
    p[n++] = (sql_param_t){ "liv_chat_id", SQL_PT_INT, { .as_int = chat_id } };
    return get_link(p, n, out);
}

int telegram_domain_user_public(int64_t usu_id, char* public_out,
                                size_t public_cap, char* name_out,
                                size_t name_cap) {
    sql_param_t p[2];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoConsulta", SQL_PT_INT,
                            { .as_int = CONS_TG_USER_PUBLIC } };
    p[n++] = (sql_param_t){ "lic_id_usuario", SQL_PT_INT, { .as_int = usu_id } };
    sql_result_t out;
    memset(&out, 0, sizeof(out));
    int rc = run_sp("procLlamadasIntegracionVinculosCons", p, n, &out);
    int ok = 0;
    if (rc == 0 && out.nrows > 0) {
        copy_str(public_out, public_cap, row_str(&out, 0, "usu_id_public"));
        copy_str(name_out, name_cap, row_str(&out, 0, "usu_nombre"));
        ok = 1;
    }
    sql_result_free(&out);
    return ok;
}

int telegram_domain_unlink_user(int64_t usu_id) {
    sql_param_t p[2];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoProceso", SQL_PT_INT,
                            { .as_int = PROC_TG_UNLINK } };
    p[n++] = (sql_param_t){ "lic_id_usuario", SQL_PT_INT, { .as_int = usu_id } };
    sql_result_t out;
    memset(&out, 0, sizeof(out));
    int rc = run_sp("procLlamadasIntegracionVinculosProc", p, n, &out);
    int ok = (rc == 0) && row_ok(&out);
    sql_result_free(&out);
    return ok;
}

int telegram_domain_unlink_chat(int64_t chat_id) {
    sql_param_t p[2];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoProceso", SQL_PT_INT,
                            { .as_int = PROC_TG_UNLINK } };
    p[n++] = (sql_param_t){ "liv_chat_id", SQL_PT_INT, { .as_int = chat_id } };
    sql_result_t out;
    memset(&out, 0, sizeof(out));
    int rc = run_sp("procLlamadasIntegracionVinculosProc", p, n, &out);
    int ok = (rc == 0) && row_ok(&out);
    sql_result_free(&out);
    return ok;
}

int telegram_domain_external_link(int64_t usu_id, int type,
                                  const char* value) {
    if (!g_sql || !value || !*value) return 0;
    sql_param_t p[4];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoProceso", SQL_PT_INT,
                            { .as_int = PROC_INT_LINK } };
    p[n++] = (sql_param_t){ "lie_id_usuario", SQL_PT_INT,
                            { .as_int = usu_id } };
    p[n++] = (sql_param_t){ "lie_tipo", SQL_PT_INT, { .as_int = type } };
    p[n++] = (sql_param_t){ "lie_valor", SQL_PT_STRING, { .as_string = value } };
    sql_result_t out;
    memset(&out, 0, sizeof(out));
    int rc = run_sp("procLlamadasIntegracionExternaProc", p, n, &out);
    int ok = (rc == 0) && row_ok(&out);
    sql_result_free(&out);
    return ok;
}

int telegram_domain_external_unlink(int64_t usu_id, int type,
                                    const char* value) {
    if (!g_sql) return 0;
    sql_param_t p[4];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoProceso", SQL_PT_INT,
                            { .as_int = PROC_INT_UNLINK } };
    p[n++] = (sql_param_t){ "lie_id_usuario", SQL_PT_INT, { .as_int = usu_id } };
    p[n++] = (sql_param_t){ "lie_tipo", type > 0 ? SQL_PT_INT : SQL_PT_NULL,
                            { .as_int = type } };
    p[n++] = (sql_param_t){ "lie_valor",
                            (value && *value) ? SQL_PT_STRING : SQL_PT_NULL,
                            { .as_string = value } };
    sql_result_t out;
    memset(&out, 0, sizeof(out));
    int rc = run_sp("procLlamadasIntegracionExternaProc", p, n, &out);
    int ok = (rc == 0) && row_ok(&out);
    sql_result_free(&out);
    return ok;
}

int telegram_domain_external_lookup(int type, const char* value,
                                    int64_t* usu_id_out,
                                    char* public_out, size_t public_cap,
                                    char* name_out, size_t name_cap) {
    if (!g_sql || !value || !*value) return 0;
    sql_param_t p[3];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoConsulta", SQL_PT_INT,
                            { .as_int = CONS_INT_LOOKUP } };
    p[n++] = (sql_param_t){ "lie_tipo", SQL_PT_INT, { .as_int = type } };
    p[n++] = (sql_param_t){ "lie_valor", SQL_PT_STRING, { .as_string = value } };
    sql_result_t out;
    memset(&out, 0, sizeof(out));
    int rc = run_sp("procLlamadasIntegracionExternaCons", p, n, &out);
    int ok = 0;
    if (rc == 0 && out.nrows > 0) {
        const char* s = row_str(&out, 0, "success");
        if (!(s && strcmp(s, "false") == 0)) {
            if (usu_id_out)
                *usu_id_out = row_i64(&out, 0, "lie_id_usuario", 0);
            copy_str(public_out, public_cap,
                     row_str(&out, 0, "usu_id_public"));
            copy_str(name_out, name_cap, row_str(&out, 0, "usu_nombre"));
            ok = 1;
        }
    }
    sql_result_free(&out);
    return ok;
}
