#define _POSIX_C_SOURCE 200809L

#include "users_device_domain.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "configuration.h"
#include "cws/cws.h"

#define USERS_DB "soda_stream"
#define PROC_SET   1
#define PROC_CLEAR 2

static sql_eject_t* g_sql = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int run_sp(const char* sp, const sql_param_t* params, size_t n,
                  sql_result_t* out) {
    if (!g_sql || !out) return -1;
    pthread_mutex_lock(&g_lock);
    int rc = sql_eject_store(g_sql, sp, USERS_DB, params, n, out);
    pthread_mutex_unlock(&g_lock);
    return rc == CWS_OK ? 0 : -1;
}

static int row_ok(const sql_result_t* out) {
    if (!out || out->nrows == 0) return 0;
    for (size_t c = 0; c < out->ncols; c++) {
        if (out->columns[c].name &&
            strcmp(out->columns[c].name, "success") == 0) {
            const sql_value_t* v = &out->rows[0].cols[c];
            return !v->is_null && v->type == SQL_COL_STRING &&
                   v->val.as_string && strcmp(v->val.as_string, "true") == 0;
        }
    }
    return 0;
}

void users_device_domain_init(void) {
    app_config_t* cfg = configuration_new();
    if (!cfg) return;
    sql_eject_t* se = sql_eject_new(cfg);
    configuration_free(cfg);
    if (!se) return;
    sql_eject_free(g_sql);
    g_sql = se;
}

void users_device_domain_shutdown(void) {
    pthread_mutex_lock(&g_lock);
    sql_eject_free(g_sql);
    g_sql = NULL;
    pthread_mutex_unlock(&g_lock);
}

static int set_or_clear(int64_t usu_id, const char* device_id, int proc) {
    if (!g_sql || !usu_id) return 0;
    sql_param_t p[3];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoProceso", SQL_PT_INT, { .as_int = proc } };
    p[n++] = (sql_param_t){ "usu_id", SQL_PT_INT, { .as_int = usu_id } };
    p[n++] = (sql_param_t){ "usu_device_id",
                            device_id ? SQL_PT_STRING : SQL_PT_NULL,
                            { .as_string = device_id } };
    sql_result_t out;
    memset(&out, 0, sizeof(out));
    int rc = run_sp("procUsersDeviceProc", p, n, &out);
    int ok = (rc == 0) && row_ok(&out);
    sql_result_free(&out);
    return ok;
}

int users_device_set(int64_t usu_id, const char* device_id) {
    if (!device_id || !*device_id || strlen(device_id) > 255) return 0;
    return set_or_clear(usu_id, device_id, PROC_SET);
}

int users_device_clear(int64_t usu_id) {
    return set_or_clear(usu_id, NULL, PROC_CLEAR);
}

sql_result_t* users_device_get(int64_t usu_id) {
    if (!g_sql || !usu_id) return NULL;
    sql_param_t p[2];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoConsulta", SQL_PT_INT, { .as_int = 1 } };
    p[n++] = (sql_param_t){ "usu_id", SQL_PT_INT, { .as_int = usu_id } };
    sql_result_t* out = (sql_result_t*)calloc(1, sizeof(*out));
    if (!out) return NULL;
    if (run_sp("procUsersDeviceCons", p, n, out) != 0) {
        sql_result_free(out);
        return NULL;
    }
    return out;
}
