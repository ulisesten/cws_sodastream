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

/* --- main --------------------------------------------------------------- */

int main(void) {
    g_app = cws_app_new();
    if (!g_app) return 1;

    /* Load .env (PORT, etc.) */
    cws_app_env_file(g_app, ".env");

    /* Read PORT from .env, fallback to 8181 */
    const char* port_str = cws_app_env_get_or(g_app, "PORT", "8181");
    int port = atoi(port_str);
    if (port < 1 || port > 65535) port = 8181;

    cws_app_port(g_app, port);
    cws_app_bind(g_app, "0.0.0.0");
    cws_app_pin(g_app, 1);
    cws_app_workers(g_app, 0);
    cws_app_log_level(g_app, CWS_LOG_INFO);

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

    fprintf(stderr, "cws_sodastream starting on port %d (PORT from .env)\n", port);
    int rc = cws_app_run(g_app);
    cws_app_free(g_app);
    return rc == CWS_OK ? 0 : 1;
}