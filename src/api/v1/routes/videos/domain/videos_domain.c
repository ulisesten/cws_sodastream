/*
 * videos_domain.c — acceso a datos del módulo de videos.
 *
 * Espejo en C de streaming_server/src/server/app/routes/api/v1/videos/domain/videos_domain.js.
 * Cada handler del dominio construye sus parámetros, ejecuta el stored
 * procedure vía core/sql_eject (ODBC) y responde el recordset como JSON.
 */

#include "videos_domain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "configuration.h"
#include "sql_eject.h"

#define VIDEOS_DB "soda_stream"

/* Códigos de tipoConsulta de procCatVideosCons (SMALLINT, ver sql/). */
#define CAT_SERIES_VIDEOS_CONS      1
#define CAT_VIDEO_BY_ID_CONS        2
#define CAT_VID_THUMNAIL_CONS       3
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

/* ------------------------------------------------------------------ */
/* helpers de DTO                                                      */
/* ------------------------------------------------------------------ */

/* Copia NUL-terminada de un path-param. cws_request_param() devuelve el
 * valor con su longitud pero sin terminar en el límite del segmento (para
 * params que no son el último, el valor incluye el resto del path), así que
 * aquí se copia respetando value_len. Liberar con free. */
static char* param_dup(const cws_request_t* req, const char* name) {
    if (!req || !name) return NULL;
    size_t kl = strlen(name);
    for (size_t i = 0; i < req->path_params_count; i++) {
        const cws_query_kv_t* kv = &req->path_params[i];
        if (kv->key_len != kl || memcmp(kv->key, name, kl) != 0) continue;
        char* out = (char*)malloc(kv->value_len + 1);
        if (!out) return NULL;
        memcpy(out, kv->value, kv->value_len);
        out[kv->value_len] = '\0';
        return out;
    }
    return NULL;
}

/* Valor string de una columna de la primera fila (NULL si no existe,
 * es NULL o es de otro tipo). Equivalente a data[0].<col> del DTO JS. */
static const char* field_str(const sql_result_t* out, const char* name) {
    if (!out || out->nrows == 0) return NULL;
    for (size_t c = 0; c < out->ncols; c++) {
        const char* cn = out->columns[c].name;
        if (!cn || strcmp(cn, name) != 0) continue;
        const sql_value_t* v = &out->rows[0].cols[c];
        if (v->is_null || v->type != SQL_COL_STRING || !v->val.as_string)
            return NULL;
        return v->val.as_string;
    }
    return NULL;
}

/* Valor entero de una columna de la primera fila (def si no existe). */
static int64_t field_int(const sql_result_t* out, const char* name,
                         int64_t def) {
    if (!out || out->nrows == 0) return def;
    for (size_t c = 0; c < out->ncols; c++) {
        const char* cn = out->columns[c].name;
        if (!cn || strcmp(cn, name) != 0) continue;
        const sql_value_t* v = &out->rows[0].cols[c];
        if (v->is_null || v->type != SQL_COL_INT) return def;
        return v->val.as_int;
    }
    return def;
}

/* Escapa un string para JSON (comillas, backslash y controles) en un
 * buffer malloc; liberar con free. */
static char* json_escape_dup(const char* s) {
    if (!s) s = "";
    size_t cap = strlen(s) * 6 + 3;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    char* w = out;
    *w++ = '"';
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        switch (*p) {
            case '"':  *w++ = '\\'; *w++ = '"';  break;
            case '\\': *w++ = '\\'; *w++ = '\\'; break;
            case '\n': *w++ = '\\'; *w++ = 'n';  break;
            case '\r': *w++ = '\\'; *w++ = 'r';  break;
            case '\t': *w++ = '\\'; *w++ = 't';  break;
            default:
                if (*p < 0x20) {
                    w += snprintf(w, 7, "\\u%04x", *p);
                } else {
                    *w++ = (char)*p;
                }
        }
    }
    *w++ = '"';
    *w = '\0';
    return out;
}

/*
 * Compone el DTO general de la referencia:
 *   {"msg":"<msg>","success":<success>,"error":<error>[,"data":<data>]}
 * success_json ya viene serializado (p. ej. "true", "\"true\"" o "false").
 * data_json es un array JSON ya serializado, o NULL para omitir la clave.
 * El resultado es malloc (liberar con free o pasar a body_owned).
 */
static char* dto_compose(const char* msg, const char* success_json,
                         int64_t error, const char* data_json) {
    char* esc = json_escape_dup(msg);
    if (!esc) return NULL;
    size_t cap = strlen(esc) + (data_json ? strlen(data_json) : 0) + 96;
    char* buf = (char*)malloc(cap);
    if (!buf) {
        free(esc);
        return NULL;
    }
    int n;
    if (data_json) {
        n = snprintf(buf, cap,
                     "{\"msg\":%s,\"success\":%s,\"error\":%lld,\"data\":%s}",
                     esc, success_json, (long long)error, data_json);
    } else {
        n = snprintf(buf, cap, "{\"msg\":%s,\"success\":%s,\"error\":%lld}",
                     esc, success_json, (long long)error);
    }
    free(esc);
    if (n < 0 || (size_t)n >= cap) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* ------------------------------------------------------------------ */
/* GET /api/v1/videos/:id/series/relacionados                          */
/* ------------------------------------------------------------------ */

/*
 * Videos de la serie a la que pertenece el video (procCatVideosCons,
 * tipoConsulta = CAT_SERIES_VIDEOS_CONS = 1, vid_id = :id — mismo contrato
 * que videos_domain.js get_series_videos).
 *
 * El SP devuelve los videos SIN columnas msg/success/error, así que el DTO
 * aplica sus defaults; con resultado vacío responde data: null.
 */
void get_series_videos(cws_request_t* req, cws_response_t* res) {
    if (!g_sql) {
        cws_response_send_error(res, 500);
        return;
    }
    char* id = param_dup(req, "id");
    char* end = NULL;
    long vid_id = id ? strtol(id, &end, 10) : 0;
    int valid = id && *id && end && *end == '\0';
    free(id);
    if (!valid) {
        cws_response_send_error(res, 400);
        return;
    }

    sql_param_t params[2];
    params[0].name = "tipoConsulta";
    params[0].type = SQL_PT_INT;
    params[0].val.as_int = CAT_SERIES_VIDEOS_CONS;
    params[1].name = "vid_id";
    params[1].type = SQL_PT_INT;
    params[1].val.as_int = vid_id;

    sql_result_t out;
    int rc = sql_eject_store(g_sql, "procCatVideosCons", VIDEOS_DB, params, 2,
                             &out);
    if (rc != CWS_OK) {
        cws_response_send_error(res, 500);
        return;
    }

    /* DTO get_series_videos_response: sin filas -> data null y "Sin
     * resultados."; con filas -> msg/success/error de la primera fila con
     * defaults, data = array completo. */
    char* body;
    if (out.nrows == 0) {
        body = dto_compose("Sin resultados.", "true", 0, "null");
    } else {
        const char* msg = field_str(&out, "msg");
        const char* success = field_str(&out, "success");
        int64_t error = field_int(&out, "error", 0);
        char* arr = sql_result_to_json(&out);
        body = dto_compose(msg ? msg : "Se obtuvieron los videos relacionados "
                                      "correctamente.",
                           success ? success : "true", error,
                           arr ? arr : "[]");
        free(arr);
    }
    sql_result_free(&out);
    if (!body) {
        cws_response_send_error(res, 500);
        return;
    }
    cws_response_body_owned(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

/* ------------------------------------------------------------------ */
/* PUT /api/v1/videos/:id/views                                        */
/* ------------------------------------------------------------------ */

/*
 * Incrementa el contador de vistas (procCatVideosProc, tipoRegistro =
 * "CAT_VIDEOS_VIEW", vid_id = :id — mismo contrato que videos_domain.js
 * insert_view). Responde el DTO general (msg/success/error).
 */
void insert_view(cws_request_t* req, cws_response_t* res) {
    if (!g_sql) {
        cws_response_send_error(res, 500);
        return;
    }
    char* id = param_dup(req, "id");
    char* end = NULL;
    long vid_id = id ? strtol(id, &end, 10) : 0;
    int valid = id && *id && end && *end == '\0';
    free(id);
    if (!valid) {
        cws_response_send_error(res, 400);
        return;
    }

    sql_param_t params[2];
    params[0].name = "tipoRegistro";
    params[0].type = SQL_PT_STRING;
    params[0].val.as_string = "CAT_VIDEOS_VIEW";
    params[1].name = "vid_id";
    params[1].type = SQL_PT_INT;
    params[1].val.as_int = vid_id;

    sql_result_t out;
    int rc = sql_eject_store(g_sql, "procCatVideosProc", VIDEOS_DB, params, 2,
                             &out);
    if (rc != CWS_OK) {
        cws_response_send_error(res, 500);
        return;
    }

    /* DTO general_response: sin filas -> error genérico; con filas ->
     * msg/success/error crudos de la primera fila. */
    char* body;
    if (out.nrows == 0) {
        body = dto_compose("Error al procesar información.", "false", 1, NULL);
    } else {
        const char* msg = field_str(&out, "msg");
        const char* success = field_str(&out, "success");
        int64_t error = field_int(&out, "error", 0);
        /* El SP devuelve success como varchar ('true'/'false'); la
         * referencia lo emite como string JSON: "success":"true". */
        char success_json[16];
        snprintf(success_json, sizeof(success_json), "\"%s\"",
                 success ? success : "false");
        body = dto_compose(msg ? msg : "", success_json, error, NULL);
    }
    sql_result_free(&out);
    if (!body) {
        cws_response_send_error(res, 500);
        return;
    }
    cws_response_body_owned(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

/* ------------------------------------------------------------------ */
/* GET /api/v1/videos/thumbnails/:name                                 */
/* ------------------------------------------------------------------ */

/* 1 si path existe y es un archivo regular. */
static int thumb_file_exists(const char* path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Content-Type por extensión del archivo de imagen. */
static const char* thumb_mime(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++;
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(ext, "png") == 0) return "image/png";
    if (strcasecmp(ext, "gif") == 0) return "image/gif";
    if (strcasecmp(ext, "webp") == 0) return "image/webp";
    return "application/octet-stream";
}

/*
 * Resuelve la imagen de la miniatura en el filesystem.
 *
 * La BD guarda rutas relativas (p. ej. ../../nas/images/videos/tmb-x.jpg) y
 * los archivos viven en API_NAS: se usa el nombre base de thu_path con
 * API_NAS como directorio (una ruta absoluta existente se respeta tal cual).
 * Igual que la referencia, prueba primero la extensión pedida (o la ruta tal
 * cual si no hay ext) y luego los fallbacks jpg,jpeg,png,gif,webp.
 * Devuelve malloc (liberar con free) o NULL si no hay candidato.
 */
static char* thumb_resolve(const char* thu_path, const char* ext) {
    static const char* const fallbacks[] = {"jpg", "jpeg", "png", "gif",
                                            "webp"};
    if (!thu_path || !*thu_path) return NULL;

    /* Directorio base: API_NAS (env central); rutas absolutas tal cual. */
    char dir[512];
    if (thu_path[0] == '/') {
        /* Absoluta: separa dirname/basename de thu_path. */
        const char* slash = strrchr(thu_path, '/');
        size_t dlen = slash ? (size_t)(slash - thu_path) : 1;
        if (dlen == 0) dlen = 1;
        if (dlen >= sizeof(dir)) return NULL;
        memcpy(dir, thu_path, dlen);
        dir[dlen] = '\0';
    } else {
        const char* nas = cfg_getenv("API_NAS");
        snprintf(dir, sizeof(dir), "%s", (nas && *nas) ? nas : ".");
    }

    /* Nombre base sin extensión. */
    const char* slash = strrchr(thu_path, '/');
    const char* base = slash ? slash + 1 : thu_path;
    const char* dot = strrchr(base, '.');
    size_t stem_len = dot ? (size_t)(dot - base) : strlen(base);
    if (stem_len == 0 || stem_len >= 256) return NULL;
    char stem[256];
    memcpy(stem, base, stem_len);
    stem[stem_len] = '\0';

    /* Candidatos: extensión pedida primero; luego fallbacks (saltando la
     * pedida). Sin ext: la ruta tal cual y luego fallbacks. */
    char cand[768];
    if (ext && *ext) {
        snprintf(cand, sizeof(cand), "%s/%s.%s", dir, stem, ext);
        if (thumb_file_exists(cand)) return strdup(cand);
    } else {
        snprintf(cand, sizeof(cand), "%s/%s", dir, base);
        if (thumb_file_exists(cand)) return strdup(cand);
    }
    for (size_t i = 0; i < sizeof(fallbacks) / sizeof(fallbacks[0]); i++) {
        if (ext && *ext && strcasecmp(fallbacks[i], ext) == 0) continue;
        snprintf(cand, sizeof(cand), "%s/%s.%s", dir, stem, fallbacks[i]);
        if (thumb_file_exists(cand)) return strdup(cand);
    }
    return NULL;
}

/*
 * Sirve la imagen de la miniatura. name puede venir como
 * "<thu_id_public>.<ext>" o "<thu_id_public>" (mismo contrato que
 * streaming_server: GET /thumbnails/:thu_id_public.:ext y /:thu_id_public).
 *
 * Ejecuta procCatVideosCons con tipoConsulta = CAT_VID_THUMNAIL_CONS = 3 y
 * thu_id_public; resuelve el archivo en API_NAS y responde sendfile con el
 * Content-Type por extensión.
 */
void get_thumbnail(cws_request_t* req, cws_response_t* res) {
    const char* name = cws_request_param(req, "name");
    if (!name || !*name) {
        cws_response_send_error(res, 400);
        return;
    }
    if (!g_sql) {
        cws_response_send_error(res, 500);
        return;
    }

    /* Separa "<thu_id_public>" de la extensión opcional (último punto). */
    char* id_public = strdup(name);
    if (!id_public) {
        cws_response_send_error(res, 500);
        return;
    }
    char* dot = strrchr(id_public, '.');
    char* ext = NULL;
    if (dot && dot != id_public && *id_public) {
        *dot = '\0';
        ext = dot + 1;
    }
    if (!*id_public) {
        free(id_public);
        cws_response_send_error(res, 400);
        return;
    }

    sql_param_t params[2];
    params[0].name = "tipoConsulta";
    params[0].type = SQL_PT_INT;
    params[0].val.as_int = CAT_VID_THUMNAIL_CONS;
    params[1].name = "thu_id_public";
    params[1].type = SQL_PT_STRING;
    params[1].val.as_string = id_public;

    sql_result_t out;
    int rc = sql_eject_store(g_sql, "procCatVideosCons", VIDEOS_DB, params, 2,
                             &out);
    free(id_public);
    if (rc != CWS_OK) {
        sql_result_free(&out);
        cws_response_send_error(res, 500);
        return;
    }
    if (out.nrows == 0) {
        sql_result_free(&out);
        /* Igual que la referencia: JSON de error con status 200. */
        char* body =
            dto_compose("Ocurrió un error al consultar la imagen.", "false", 1,
                        NULL);
        if (!body) {
            cws_response_send_error(res, 500);
            return;
        }
        cws_response_status(res, 200);
        cws_response_body_owned(res, body, strlen(body),
                                CWS_MT_APPLICATION_JSON);
        cws_response_send(res);
        return;
    }

    const char* thu_path = field_str(&out, "thu_path");
    char* resolved = thu_path ? thumb_resolve(thu_path, ext) : NULL;
    sql_result_free(&out);
    if (!resolved) {
        char* body = dto_compose("Imagen no encontrada", "false", 1, NULL);
        if (!body) {
            cws_response_send_error(res, 500);
            return;
        }
        cws_response_status(res, 404);
        cws_response_body_owned(res, body, strlen(body),
                                CWS_MT_APPLICATION_JSON);
        cws_response_send(res);
        return;
    }

    cws_response_header(res, "Content-Type", "%s", thumb_mime(resolved));
    cws_response_sendfile_ex(res, resolved, "application/octet-stream", 0);
    free(resolved);
}
