/*
 * mobile_routes.c — rutas de autenticación móvil.
 *
 * Los handlers delegan en AuthorizationMobile (core/authorization_mobile),
 * que a su vez usa jwt_mobile (tokens GOST del canal móvil, sin cookies,
 * sin CSRF y sin validar IP).
 */

#include "mobile_routes.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "authorization_mobile.h"

static CWS_HANDLER(mobile_signin_handler) {
    authorization_mobile_signin(req, res);
}

static CWS_HANDLER(mobile_refresh_handler) {
    authorization_mobile_refresh(req, res);
}

/* GET /me — ruta protegida por el middleware Bearer. Devuelve el usuario
 * autorizado (valida el access_token móvil). */
static CWS_HANDLER(mobile_me_handler) {
    const jwt_user_t* u = authorization_mobile_request_user(req);
    if (!u) { cws_response_send_error(res, 401); return; }
    char body[512];
    int n = snprintf(body, sizeof(body),
                     "{\"usu_id\":%lld,\"usu_nombre\":\"%s\","
                     "\"usu_correo\":\"%s\"}",
                     (long long)u->usu_id,
                     u->usu_nombre ? u->usu_nombre : "",
                     u->usu_correo ? u->usu_correo : "");
    if (n < 0 || (size_t)n >= sizeof(body)) {
        cws_response_send_error(res, 500);
        return;
    }
    cws_response_body(res, body, (size_t)n, CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

cws_router_t* mobile_routes(void) {
    cws_router_t* r = cws_router_new();
    if (!r) return NULL;
    cws_router_add(r, CWS_M_POST, "/signin",        mobile_signin_handler);
    cws_router_add(r, CWS_M_POST, "/refresh_token", mobile_refresh_handler);

    /* Sub-router protegido: GET /me (Bearer), sin afectar a signin/refresh. */
    cws_router_t* prot = cws_router_new();
    if (prot) {
        cws_router_use(prot, cws_mw_authorization_mobile_verify);
        cws_router_add(prot, CWS_M_GET, "/", mobile_me_handler);
        cws_router_mount(r, "/me", prot);
    }
    return r;
}
