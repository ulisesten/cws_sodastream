/*
 * messaging_domain.c — acceso a datos del chat 1:1.
 *
 * Ejecuta procChatMensajesCons (historial) y procChatMensajesProc
 * (insertar, marcar leído) vía core/sql_eject.
 */

#define _POSIX_C_SOURCE 200809L

#include "messaging_domain.h"

#include <stdlib.h>
#include <string.h>

#include "configuration.h"

#define MESSAGING_DB "soda_stream"

/* procChatMensajesCons */
#define CONS_CHAT_MSG_HISTORY 1
/* procChatMensajesProc */
#define PROC_CHAT_MSG_INS  1
#define PROC_CHAT_MSG_READ 2

static sql_eject_t* g_sql = NULL;

void messaging_domain_init(void) {
    app_config_t* cfg = configuration_new();
    if (!cfg) return;
    sql_eject_t* se = sql_eject_new(cfg);
    configuration_free(cfg);
    if (!se) return;
    sql_eject_free(g_sql);
    g_sql = se;
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
                                      const char* texto, int64_t reply_to) {
    if (!g_sql || !texto) return NULL;

    sql_param_t params[5];
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
    params[n].val.as_string = texto;
    n++;

    params[n].name = "msg_id_respuesta";
    params[n].type = reply_to > 0 ? SQL_PT_INT : SQL_PT_NULL;
    params[n].val.as_int = reply_to;
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
