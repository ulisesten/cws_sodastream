#define _POSIX_C_SOURCE 200809L

#include "users_device_domain.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "configuration.h"
#include "cws/cws.h"

#define USERS_DB "soda_stream"
#define PROC_UPSERT     1
#define PROC_DEACTIVATE 2
#define PROC_DELETE     3
#define PROC_LOGOUT_ALL 4

#define CONS_ACTIVE 1
#define CONS_ONE    2
#define CONS_ALL    3
#define CONS_PUSH   4

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

int users_device_ok(const sql_result_t* out) {
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

static sql_result_t* call_result(const char* sp, sql_param_t* p, size_t n) {
    sql_result_t* out = (sql_result_t*)calloc(1, sizeof(*out));
    if (!out) return NULL;
    if (run_sp(sp, p, n, out) != 0) {
        sql_result_free(out);
        return NULL;
    }
    return out;
}

sql_result_t* users_devices_upsert(int64_t usu_id, const char* device_id,
                                   int plataforma, const char* nombre,
                                   const char* push_token) {
    if (!g_sql || !device_id || !*device_id || strlen(device_id) > 255)
        return NULL;
    sql_param_t p[6];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoProceso", SQL_PT_INT,
                            { .as_int = PROC_UPSERT } };
    p[n++] = (sql_param_t){ "usd_id_usuario", SQL_PT_INT,
                            { .as_int = usu_id } };
    p[n++] = (sql_param_t){ "usd_device_id", SQL_PT_STRING,
                            { .as_string = device_id } };
    p[n++] = (sql_param_t){ "usd_plataforma",
                            plataforma > 0 ? SQL_PT_INT : SQL_PT_NULL,
                            { .as_int = plataforma } };
    p[n++] = (sql_param_t){ "usd_nombre",
                            (nombre && *nombre) ? SQL_PT_STRING : SQL_PT_NULL,
                            { .as_string = nombre } };
    p[n++] = (sql_param_t){ "usd_push_token",
                            (push_token && *push_token)
                                ? SQL_PT_STRING : SQL_PT_NULL,
                            { .as_string = push_token } };
    return call_result("procUsersDevicesProc", p, n);
}

sql_result_t* users_devices_list(int64_t usu_id, int active_only) {
    sql_param_t p[2];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoConsulta", SQL_PT_INT,
                            { .as_int = active_only ? CONS_ACTIVE : CONS_ALL } };
    p[n++] = (sql_param_t){ "usd_id_usuario", SQL_PT_INT,
                            { .as_int = usu_id } };
    return call_result("procUsersDevicesCons", p, n);
}

sql_result_t* users_devices_push_tokens(int64_t usu_id) {
    sql_param_t p[2];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoConsulta", SQL_PT_INT,
                            { .as_int = CONS_PUSH } };
    p[n++] = (sql_param_t){ "usd_id_usuario", SQL_PT_INT,
                            { .as_int = usu_id } };
    return call_result("procUsersDevicesCons", p, n);
}

sql_result_t* users_devices_get(int64_t usu_id, const char* device_id) {
    if (!device_id || !*device_id) return NULL;
    sql_param_t p[3];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoConsulta", SQL_PT_INT,
                            { .as_int = CONS_ONE } };
    p[n++] = (sql_param_t){ "usd_id_usuario", SQL_PT_INT,
                            { .as_int = usu_id } };
    p[n++] = (sql_param_t){ "usd_device_id", SQL_PT_STRING,
                            { .as_string = device_id } };
    return call_result("procUsersDevicesCons", p, n);
}

static int proc_int(int64_t usu_id, const char* device_id, int type) {
    sql_param_t p[3];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoProceso", SQL_PT_INT, { .as_int = type } };
    p[n++] = (sql_param_t){ "usd_id_usuario", SQL_PT_INT,
                            { .as_int = usu_id } };
    p[n++] = (sql_param_t){ "usd_device_id",
                            (device_id && *device_id)
                                ? SQL_PT_STRING : SQL_PT_NULL,
                            { .as_string = device_id } };
    sql_result_t* r = call_result("procUsersDevicesProc", p, n);
    int ok = users_device_ok(r);
    sql_result_free(r);
    return ok;
}

int users_devices_deactivate(int64_t usu_id, const char* device_id) {
    return proc_int(usu_id, device_id, PROC_DEACTIVATE);
}

int users_devices_delete(int64_t usu_id, const char* device_id) {
    return proc_int(usu_id, device_id, PROC_DELETE);
}

int users_devices_logout_all(int64_t usu_id) {
    return proc_int(usu_id, NULL, PROC_LOGOUT_ALL);
}
