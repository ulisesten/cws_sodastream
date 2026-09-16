/*
 * cws_sodastream — example app using the cws framework as a git submodule.
 *
 * Loads .env, applies middleware, mounts sub-routers.
 */

#include "cws/cws.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "api/v1/routes/videos/videos_routes.h"
#include "api/v1/routes/videos/domain/videos_domain.h"
#include "api/v1/routes/users/users_routes.h"
#include "api/v1/routes/users/domain/users_domain.h"
#include "api/v1/routes/mobile/mobile_routes.h"
#include "authorization.h"
#include "authorization_mobile.h"
#include "configuration.h"

static cws_app_t* g_app = NULL;

/* --- handlers ----------------------------------------------------------- */

static CWS_HANDLER(root_handler) {
    (void)req;
    const char* body = "{\"service\":\"cws_sodastream\",\"status\":\"running\"}";
    cws_response_body(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

static CWS_HANDLER(health_handler) {
    (void)req;
    const char* body = "{\"status\":\"ok\"}";
    cws_response_body(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

static CWS_HANDLER(metrics_handler) {
    (void)req;
    const cws_metrics_t* m = cws_app_metrics(g_app);
    if (!m) { cws_response_send_error(res, 500); return; }
    char* buf = NULL; size_t len = 0;
    if (cws_metrics_render(m, &buf, &len) != CWS_OK || !buf) {
        cws_response_send_error(res, 500); return;
    }
    cws_response_body_owned(res, buf, len, CWS_MT_TEXT_PLAIN);
    cws_response_send(res);
}

/* --- HLS estático ---------------------------------------------------------
 * Emula: app.use("/hls/videos", express.static(dir, { setHeaders }))
 * Define Content-Type y Cache-Control según la extensión del archivo.
 * ------------------------------------------------------------------------- */

static void hls_set_headers(cws_response_t* res, const char* file_path,
                            void* user) {
    (void)user;
    const char* ext = strrchr(file_path, '.');
    if (!ext) return;
    if (strcasecmp(ext, ".m3u8") == 0) {
        cws_response_header(res, "Content-Type",
                            "%s", "application/vnd.apple.mpegurl");
        cws_response_header(res, "Cache-Control", "%s", "no-cache");
    } else if (strcasecmp(ext, ".ts") == 0) {
        cws_response_header(res, "Content-Type", "%s", "video/mp2t");
        cws_response_header(res, "Cache-Control", "%s",
                            "public, max-age=31536000");
    }
}

/* --- main --------------------------------------------------------------- */

int main(void) {
    g_app = cws_app_new();
    if (!g_app) return 1;

    /* Configuración central: carga .env, lo exporta al entorno y expone
     * cfg_getenv(). Ya no necesita el handle del app. */
    app_config_t* cfg = configuration_new();
    if (!cfg) {
        fprintf(stderr, "configuration_new falló\n");
        cws_app_free(g_app);
        return 1;
    }

    cws_app_port(g_app, cfg->port);
    int port = cfg->port;
    cws_app_bind(g_app, "0.0.0.0");
    cws_app_pin(g_app, 1);
    cws_app_workers(g_app, 0);
    cws_app_log_level(g_app, CWS_LOG_INFO);

    /* Inicializa el acceso a datos del módulo de videos */
    videos_domain_init();

    /* Servicios de sesión: authorization (tokens GOST + signin) y
     * users_domain (DAO de procUsersProc/Cons). */
    if (!authorization_init() || !authorization_mobile_init()) {
        fprintf(stderr,
                "No se pudo inicializar auth: revisa SECRET_KEY y X_VECTOR "
                "(8 palabras hex separadas por coma) en .env\n");
        cws_app_free(g_app);
        return 1;
    }
    users_domain_init();

    /* Autenticación móvil (sin cookies): jwt_mobile + authorization_mobile. */


    /* Global middleware: logger + CORS on every request */
    cws_app_use(g_app, cws_mw_logger);
    cws_app_use(g_app, cws_mw_cors);

    /* App-level routes */
    CWS_GET(g_app, "/",          root_handler);
    CWS_GET(g_app, "/healthz",   health_handler);
    CWS_GET(g_app, "/metrics",   metrics_handler);

    /* Sub-router mounted at /videos — videos_routes() defines its own
     * routes relative to the mount point:
     *   GET /videos/         -> get_all_videos
     *   GET /videos/:id      -> get_video_by_id
     */
    cws_app_mount(g_app, "/api/v1/videos", videos_routes());

    /* Sub-router del módulo users — rutas relativas al punto de montaje:
     *   POST /api/v1/users/signin -> login (emite cookies)
     *   POST /api/v1/users/       -> alta de usuario
     */
    cws_app_mount(g_app, "/api/v1/users", users_routes());

    /* Router de autenticación móvil (sin cookies), namespace propio:
     *   POST /api/v1/auth/mobile/signin        -> tokens access/refresh
     *   POST /api/v1/auth/mobile/refresh_token -> nuevo access (X-Refresh-Token)
     *   GET  /api/v1/auth/mobile/me            -> protegido (Bearer)
     */
    cws_app_mount(g_app, "/api/v1/auth/mobile", mobile_routes());

    /* Servidor estático de HLS: sirve los archivos de HLS_DIR bajo
     * /hls/videos con Content-Type/Cache-Control por extensión. */
    {
        const char* hls_dir = cfg_getenv("HLS_DIR");
        if (!hls_dir || !*hls_dir) hls_dir = "public/hls/videos";
        cws_static_options_t opts = {
            .prefix = "/hls/videos",
            .dir = hls_dir,
            .index = NULL,
            .set_headers = hls_set_headers,
            .user = NULL,
        };
        cws_app_static_mount(g_app, &opts);
    }

    configuration_free(cfg);
    fprintf(stderr, "cws_sodastream starting on port %d (PORT from .env)\n",
            port);
    int rc = cws_app_run(g_app);
    cws_app_free(g_app);
    return rc == CWS_OK ? 0 : 1;
}