/*
 * releases_routes.c — endpoint de releases de la aplicación.
 *
 * Espejo en C de streaming_server/src/server/app/routes/api/v1/releases/.
 * GET /api/v1/app/releases ejecuta `procCatAppReleasesCons` (tipoConsulta =
 * CONS_RELEASES_CONS = 1) y responde el DTO releases_obtener_response:
 *   200 {"success":true,"error":0,"msg":"Releases obtenidos exitosamente",
 *        "data":[rel_id, rel_version, rel_path, rel_type, rel_date,
 *                rel_description]}
 *   404 {"success":false,"error":1,"msg":"No se encontraron releases",
 *        "data":[]}   (sin filas)
 *   500 en error de BD (catch de la referencia).
 */

#include "releases_routes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "configuration.h"
#include "sql_eject.h"

#define RELEASES_DB "soda_stream"

/* Código de tipoConsulta de procCatAppReleasesCons (ver sql/). */
#define CONS_RELEASES_CONS 1

static sql_eject_t* g_sql = NULL;

void releases_routes_init(void) {
    app_config_t* cfg = configuration_new();
    if (!cfg) return;
    sql_eject_t* se = sql_eject_new(cfg);
    configuration_free(cfg);
    if (!se) return;
    if (g_sql) sql_eject_free(g_sql);
    g_sql = se;
}

static CWS_HANDLER(get_all_releases_handler) {
    (void)req;

    if (!g_sql) {
        cws_response_send_error(res, 500);
        return;
    }

    sql_param_t params[1];
    params[0].name = "tipoConsulta";
    params[0].type = SQL_PT_INT;
    params[0].val.as_int = CONS_RELEASES_CONS;

    sql_result_t out;
    int rc = sql_eject_store(g_sql, "procCatAppReleasesCons", RELEASES_DB,
                             params, 1, &out);
    if (rc != CWS_OK) {
        cws_response_send_error(res, 500);
        return;
    }

    if (out.nrows == 0) {
        sql_result_free(&out);
        const char* body = "{\"success\":false,\"error\":1,"
                           "\"msg\":\"No se encontraron releases\","
                           "\"data\":[]}";
        cws_response_status(res, 404);
        cws_response_body(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
        cws_response_send(res);
        return;
    }

    char* arr = sql_result_to_json(&out);
    sql_result_free(&out);
    if (!arr) {
        cws_response_send_error(res, 500);
        return;
    }

    /* DTO releases_obtener_response: data con las columnas rel_* tal cual
     * las serializa sql_result_to_json. */
    size_t cap = strlen(arr) + 128;
    char* body = (char*)malloc(cap);
    if (!body) {
        free(arr);
        cws_response_send_error(res, 500);
        return;
    }
    snprintf(body, cap,
             "{\"success\":true,\"error\":0,"
             "\"msg\":\"Releases obtenidos exitosamente\",\"data\":%s}",
             arr);
    free(arr);
    cws_response_body_owned(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

cws_router_t* releases_routes(void) {
    cws_router_t* r = cws_router_new();
    if (!r) return NULL;

    cws_router_add(r, CWS_M_GET, "/releases", get_all_releases_handler);

    return r;
}
