/*
 * videos_domain.c — acceso a datos del módulo de videos.
 *
 * Espejo en C de streaming_server/src/server/app/routes/api/v1/videos/domain/videos_domain.js.
 * Cada handler del dominio construye sus parámetros, ejecuta el stored
 * procedure vía core/sql_eject (ODBC) y responde el recordset como JSON.
 */

#include "videos_domain.h"

#include <stdio.h>
#include <string.h>

#include "configuration.h"
#include "sql_eject.h"

#define VIDEOS_DB "soda_stream"

/* Códigos de tipoConsulta de procCatVideosCons (SMALLINT, ver sql/). */
#define CAT_VIDEO_BY_ID_CONS        2
#define CAT_VIDEOS_CONS             4
#define CAT_VIDEOS_MAS_VISTOS_CONS  7

static sql_eject_t* g_sql = NULL;

void videos_domain_init(void) {
    app_config_t* cfg = configuration_new();
    if (!cfg) return;
    sql_eject_t* se = sql_eject_new(cfg);
    configuration_free(cfg);
    if (!se) return;
    if (g_sql) sql_eject_free(g_sql);
    g_sql = se;
}

/*
 * GET /api/v1/videos
 *
 * Listado completo de videos (procCatVideosCons, tipoConsulta =
 * CAT_VIDEOS_CONS = 4). Resultado tipado serializado como JSON.
 */
void get_all_videos(cws_request_t* req, cws_response_t* res) {
    (void)req;

    if (!g_sql) {
        cws_response_send_error(res, 500);
        return;
    }

    sql_param_t params[1];
    params[0].name = "tipoConsulta";
    params[0].type = SQL_PT_INT;
    params[0].val.as_int = CAT_VIDEOS_CONS;

    sql_result_t out;
    int rc = sql_eject_store(g_sql, "procCatVideosCons", VIDEOS_DB, params, 1,
                             &out);
    if (rc != CWS_OK) {
        cws_response_send_error(res, 500);
        return;
    }

    char* json = sql_result_to_json(&out);
    sql_result_free(&out);
    if (!json) {
        cws_response_send_error(res, 500);
        return;
    }
    cws_response_body_owned(res, json, strlen(json), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

/*
 * GET /api/v1/videos/popular
 *
 * Listado de videos más vistos (tipoConsulta = CAT_VIDEOS_MAS_VISTOS_CONS = 7).
 */
void get_popular_videos(cws_request_t* req, cws_response_t* res) {
    (void)req;

    if (!g_sql) {
        cws_response_send_error(res, 500);
        return;
    }

    sql_param_t params[1];
    params[0].name = "tipoConsulta";
    params[0].type = SQL_PT_INT;
    params[0].val.as_int = CAT_VIDEOS_MAS_VISTOS_CONS;

    sql_result_t out;
    int rc = sql_eject_store(g_sql, "procCatVideosCons", VIDEOS_DB, params, 1, &out);
    
    if (rc != CWS_OK) {
        cws_response_send_error(res, 500);
        return;
    }

    char* json = sql_result_to_json(&out);
    sql_result_free(&out);
    if (!json) {
        cws_response_send_error(res, 500);
        return;
    }
    cws_response_body_owned(res, json, strlen(json), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

/*
 * GET /api/v1/videos/:id
 *
 * Obtiene un video por su id público. Ejecuta `procCatVideosCons` con
 * tipoConsulta = CAT_VIDEO_BY_ID_CONS y vid_id_public = :id (mismo contrato
 * que videos_domain.js video_get_by_id). El resultado tipado se serializa a
 * JSON y se entrega como body de la respuesta.
 */
void get_video_by_id(cws_request_t* req, cws_response_t* res) {
    const char* id = cws_request_param(req, "id");
    if (!id) {
        cws_response_send_error(res, 400);
        return;
    }

    if (!g_sql) {
        cws_response_send_error(res, 500);
        return;
    }

    sql_param_t params[2];
    params[0].name = "tipoConsulta";
    params[0].type = SQL_PT_INT;
    params[0].val.as_int = CAT_VIDEO_BY_ID_CONS;
    params[1].name = "vid_id_public";
    params[1].type = SQL_PT_STRING;
    params[1].val.as_string = id;

    sql_result_t out;
    int rc = sql_eject_store(g_sql, "procCatVideosCons", VIDEOS_DB, params, 2,
                             &out);
    if (rc != CWS_OK) {
        cws_response_send_error(res, 500);
        return;
    }

    char* json = sql_result_to_json(&out);
    sql_result_free(&out);
    if (!json) {
        cws_response_send_error(res, 500);
        return;
    }
    cws_response_body_owned(res, json, strlen(json), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}
