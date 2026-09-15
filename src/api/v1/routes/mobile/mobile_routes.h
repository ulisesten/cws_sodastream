#ifndef MOBILE_ROUTES_H
#define MOBILE_ROUTES_H

#include "cws/cws.h"

/**
 * Router de autenticación móvil (sin cookies). Rutas relativas al punto de
 * montaje:
 *
 *   POST /signin         -> authorization_mobile_signin (devuelve tokens
 *                           access/refresh en el body)
 *   POST /refresh_token  -> authorization_mobile_refresh (nuevo access vía
 *                           header X-Refresh-Token)
 */
cws_router_t* mobile_routes(void);

#endif
