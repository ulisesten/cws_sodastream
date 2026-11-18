#define _POSIX_C_SOURCE 200809L

#include "push_outbox_domain.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "configuration.h"
#include "sql_eject.h"
#include "cws/cws.h"

#define PUSH_DB "soda_stream"
#define PROC_ENQUEUE 1
#define PROC_MARK_SENT 2
#define PROC_MARK_RETRY 3
#define PROC_MARK_DEAD 4
#define PROC_CLAIM 5

static sql_eject_t* g_sql = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int run_sp(const char* sp, const sql_param_t* params, size_t n,
                  sql_result_t* out) {
    if (!g_sql || !out) return -1;
    pthread_mutex_lock(&g_lock);
    int rc = sql_eject_store(g_sql, sp, PUSH_DB, params, n, out);
    pthread_mutex_unlock(&g_lock);
    return rc == CWS_OK ? 0 : -1;
}

static const sql_value_t* row_field(const sql_result_t* out, size_t row,
                                    const char* name) {
    if (!out || row >= out->nrows) return NULL;
    for (size_t c = 0; c < out->ncols; c++) {
        if (out->columns[c].name && strcmp(out->columns[c].name, name) == 0)
            return &out->rows[row].cols[c];
    }
    return NULL;
}

static const char* row_str(const sql_result_t* out, size_t row,
                           const char* name) {
    const sql_value_t* v = row_field(out, row, name);
    if (!v || v->is_null || v->type != SQL_COL_STRING) return NULL;
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
    return s && strcmp(s, "true") == 0;
}

static void copy_str(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void push_outbox_domain_init(void) {
    app_config_t* cfg = configuration_new();
    if (!cfg) return;
    sql_eject_t* se = sql_eject_new(cfg);
    configuration_free(cfg);
    if (!se) return;
    sql_eject_free(g_sql);
    g_sql = se;
}

void push_outbox_domain_shutdown(void) {
    pthread_mutex_lock(&g_lock);
    sql_eject_free(g_sql);
    g_sql = NULL;
    pthread_mutex_unlock(&g_lock);
}

int push_outbox_enqueue(int64_t usu_id, const char* device_id,
                        const char* token, const char* type,
                        const char* payload, const char* collapse,
                        const char* priority, int ttl,
                        const char* event_key) {
    if (!g_sql || !device_id || !token || !type || !payload || !event_key)
        return 0;
    sql_param_t p[9];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoProceso", SQL_PT_INT,
                            { .as_int = PROC_ENQUEUE } };
    p[n++] = (sql_param_t){ "po_event_key", SQL_PT_STRING,
                            { .as_string = event_key } };
    p[n++] = (sql_param_t){ "po_id_usuario", SQL_PT_INT,
                            { .as_int = usu_id } };
    p[n++] = (sql_param_t){ "po_device_id", SQL_PT_STRING,
                            { .as_string = device_id } };
    p[n++] = (sql_param_t){ "po_token", SQL_PT_STRING,
                            { .as_string = token } };
    p[n++] = (sql_param_t){ "po_type", SQL_PT_STRING, { .as_string = type } };
    p[n++] = (sql_param_t){ "po_payload", SQL_PT_STRING,
                            { .as_string = payload } };
    p[n++] = (sql_param_t){ "po_collapse",
                            (collapse && *collapse) ? SQL_PT_STRING : SQL_PT_NULL,
                            { .as_string = collapse } };
    p[n++] = (sql_param_t){ "po_priority", SQL_PT_STRING,
                            { .as_string = priority ? priority : "normal" } };
    /* ttl needs one more slot */
    sql_param_t all[10];
    memcpy(all, p, sizeof(p));
    all[n++] = (sql_param_t){ "po_ttl", SQL_PT_INT, { .as_int = ttl } };

    sql_result_t out;
    memset(&out, 0, sizeof(out));
    int rc = run_sp("procPushOutboxProc", all, n, &out);
    int ok = (rc == 0) && row_ok(&out);
    sql_result_free(&out);
    return ok;
}

int push_outbox_claim(int limit, push_outbox_item_t* out, size_t max) {
    if (!g_sql || !out || max == 0) return 0;
    sql_param_t p[2];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoProceso", SQL_PT_INT,
                            { .as_int = PROC_CLAIM } };
    p[n++] = (sql_param_t){ "po_limit", SQL_PT_INT,
                            { .as_int = limit > 0 ? limit : 20 } };
    sql_result_t res;
    memset(&res, 0, sizeof(res));
    if (run_sp("procPushOutboxProc", p, n, &res) != 0) {
        sql_result_free(&res);
        return 0;
    }
    int count = 0;
    for (size_t i = 0; i < res.nrows && (size_t)count < max; i++) {
        push_outbox_item_t* it = &out[count++];
        memset(it, 0, sizeof(*it));
        it->id = row_i64(&res, i, "po_id", 0);
        it->usu_id = row_i64(&res, i, "po_id_usuario", 0);
        copy_str(it->device_id, sizeof(it->device_id),
                 row_str(&res, i, "po_device_id"));
        copy_str(it->token, sizeof(it->token),
                 row_str(&res, i, "po_token"));
        copy_str(it->type, sizeof(it->type), row_str(&res, i, "po_type"));
        copy_str(it->payload, sizeof(it->payload),
                 row_str(&res, i, "po_payload"));
        copy_str(it->collapse, sizeof(it->collapse),
                 row_str(&res, i, "po_collapse"));
        copy_str(it->priority, sizeof(it->priority),
                 row_str(&res, i, "po_priority"));
        it->ttl = (int)row_i64(&res, i, "po_ttl", 60);
        it->attempts = (int)row_i64(&res, i, "po_attempts", 0);
    }
    sql_result_free(&res);
    return count;
}

int push_outbox_mark(int64_t id, int status, const char* error, int retry_s) {
    if (!g_sql || id == 0) return 0;
    int proc = status == 2 ? PROC_MARK_SENT
             : status == 3 ? PROC_MARK_RETRY
             : PROC_MARK_DEAD;
    sql_param_t p[4];
    size_t n = 0;
    p[n++] = (sql_param_t){ "tipoProceso", SQL_PT_INT, { .as_int = proc } };
    p[n++] = (sql_param_t){ "po_id", SQL_PT_INT, { .as_int = id } };
    p[n++] = (sql_param_t){ "po_error",
                            (error && *error) ? SQL_PT_STRING : SQL_PT_NULL,
                            { .as_string = error } };
    p[n++] = (sql_param_t){ "po_retry_s", SQL_PT_INT,
                            { .as_int = retry_s > 0 ? retry_s : 30 } };
    sql_result_t out;
    memset(&out, 0, sizeof(out));
    int rc = run_sp("procPushOutboxProc", p, n, &out);
    int ok = (rc == 0) && row_ok(&out);
    sql_result_free(&out);
    return ok;
}
