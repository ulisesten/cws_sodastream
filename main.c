/*
 * Example app demonstrating cws library usage as a submodule dependency.
 *
 * Routes:
 *   GET /                  -> 200 plain
 *   GET /healthz          -> 200 JSON
 *   GET /users/:id         -> JSON echoing the :id path param
 *   GET /metrics          -> Prometheus
 */

#include "cws/cws.h"

#include <stdio.h>
#include <string.h>

static cws_app_t* g_app = NULL;

static CWS_HANDLER(root_handler) {
    (void)req;
    const char* body = "cws_example_app running\n";
    cws_response_body(res, body, strlen(body), CWS_MT_TEXT_PLAIN);
    cws_response_send(res);
}

static CWS_HANDLER(health_handler) {
    (void)req;
    const char* body = "{\"status\":\"ok\"}";
    cws_response_body(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

static CWS_HANDLER(user_handler) {
    const char* id = cws_request_param(req, "id");
    char body[128];
    int n = snprintf(body, sizeof(body),
                     "{\"user\": {\"id\": \"%s\"}}",
                     id ? id : "null");
    if (n < 0) n = 0;
    cws_response_body(res, body, (size_t)n, CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

static CWS_HANDLER(metrics_handler) {
    (void)req;
    const cws_metrics_t* m = cws_app_metrics(g_app);
    if (!m) { cws_response_send_error(res, 500); return; }
    char* buf = NULL;
    size_t len = 0;
    if (cws_metrics_render(m, &buf, &len) != CWS_OK || !buf) {
        cws_response_send_error(res, 500);
        return;
    }
    cws_response_body_owned(res, buf, len, CWS_MT_TEXT_PLAIN);
    cws_response_send(res);
}

int main(void) {
    g_app = cws_app_new();
    if (!g_app) return 1;

    cws_app_port(g_app, 8181);
    cws_app_bind(g_app, "0.0.0.0");
    cws_app_pin(g_app, 1);
    cws_app_workers(g_app, 0);          /* auto: physical cores          */
    cws_app_log_level(g_app, CWS_LOG_INFO);

    CWS_GET(g_app, "/",            root_handler);
    CWS_GET(g_app, "/healthz",     health_handler);
    CWS_GET(g_app, "/users/:id",   user_handler);
    CWS_GET(g_app, "/metrics",     metrics_handler);

    int rc = cws_app_run(g_app);
    cws_app_free(g_app);
    return rc == CWS_OK ? 0 : 1;
}
