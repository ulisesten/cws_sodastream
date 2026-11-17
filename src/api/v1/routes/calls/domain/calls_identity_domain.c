#define _POSIX_C_SOURCE 200809L

#include "calls_identity_domain.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "configuration.h"
#include "cws/cws.h"

#define CALLS_DB "soda_stream"
#define PROC_IDENTITY 1
#define CONS_IDENTITY_LIST 1

static sql_eject_t* g_sql = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int run_sp(const char* sp, const sql_param_t* params, size_t n,
                  sql_result_t* out) {
    if (!g_sql || !out) return -1;
    pthread_mutex_lock(&g_lock);
    int rc = sql_eject_store(g_sql, sp, CALLS_DB, params, n, out);
    pthread_mutex_unlock(&g_lock);
    return rc == CWS_OK ? 0 : -1;
}

static int row_ok(const sql_result_t* out) {
    if (!out || out->nrows == 0) return 0;
    for (size_t c = 0; c < out->ncols; c++) {
        if ((out->columns[c].name &&
             strcmp(out->columns[c].name, "success") == 0)) {
            const sql_value_t* v = &out->rows[0].cols[c];
            return !v->is_null && v->type == SQL_COL_STRING &&
                   v->val.as_string && strcmp(v->val.as_string, "true") == 0;
        }
    }
    return 0;
}

void calls_identity_domain_init(void) {
    app_config_t* cfg = configuration_new();
    if (!cfg) return;
    sql_eject_t* se = sql_eject_new(cfg);
    configuration_free(cfg);
    if (!se) return;
    sql_eject_free(g_sql);
    g_sql = se;
}

void calls_identity_domain_shutdown(void) {
    pthread_mutex_lock(&g_lock);
    sql_eject_free(g_sql);
    g_sql = NULL;
    pthread_mutex_unlock(&g_lock);
}

int calls_identity_upsert(int64_t usu_id, const char* device_id,
                          const char* public_key) {
    if (!g_sql || !device_id || !*device_id || !public_key || !*public_key)
        return 0;
    sql_param_t p[4];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoProceso", SQL_PT_INT,
                            { .as_int = PROC_IDENTITY } };
    p[n++] = (sql_param_t){ "lid_id_usuario", SQL_PT_INT,
                            { .as_int = usu_id } };
    p[n++] = (sql_param_t){ "lid_dispositivo", SQL_PT_STRING,
                            { .as_string = device_id } };
    p[n++] = (sql_param_t){ "lid_pub", SQL_PT_STRING,
                            { .as_string = public_key } };
    sql_result_t out;
    memset(&out, 0, sizeof(out));
    int rc = run_sp("procLlamadasIdentidadProc", p, n, &out);
    int ok = (rc == 0) && row_ok(&out);
    sql_result_free(&out);
    return ok;
}

sql_result_t* calls_identity_list(int64_t usu_id) {
    if (!g_sql) return NULL;
    sql_param_t p[2];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoConsulta", SQL_PT_INT,
                            { .as_int = CONS_IDENTITY_LIST } };
    p[n++] = (sql_param_t){ "lid_id_usuario", SQL_PT_INT,
                            { .as_int = usu_id } };
    sql_result_t* out = (sql_result_t*)calloc(1, sizeof(*out));
    if (!out) return NULL;
    if (run_sp("procLlamadasIdentidadCons", p, n, out) != 0) {
        sql_result_free(out);
        return NULL;
    }
    return out;
}
