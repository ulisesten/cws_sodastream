#ifndef MESSAGING_ROUTES_H
#define MESSAGING_ROUTES_H

#include "cws/cws.h"

/*
 * Router de mensajería instantánea, montado en /api/v1/messaging.
 * Todas sus rutas requieren el access token móvil (Bearer).
 *
 *   POST /api/v1/messaging/setup -> habilita el chat del usuario y devuelve
 *                                   su identificador público (nanoid 12).
 *   GET  /api/v1/messaging/users/:public_id -> busca un usuario por su id
 *                                   público (devuelve usu_nombre).
 */
cws_router_t* messaging_routes(void);

#endif
