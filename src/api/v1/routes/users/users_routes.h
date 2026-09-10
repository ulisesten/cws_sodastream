#ifndef USERS_ROUTES_H
#define USERS_ROUTES_H

#include "cws/cws.h"

/**
 * Router del módulo users. Rutas relativas al punto de montaje
 * (equivalente a reference/users/routes.js):
 *
 *   POST /signin  -> user_signin (login)
 *   POST /        -> user_new   (registro)
 */
cws_router_t* users_routes(void);

#endif
