#ifndef MESSAGING_ROUTES_H
#define MESSAGING_ROUTES_H

#include "cws/cws.h"

/*
 * Router de mensajería instantánea, montado en /api/v1/messaging.
 * Todas sus rutas requieren el access token móvil (Bearer).
 *
 *   POST /api/v1/messaging/setup -> habilita el chat del usuario y devuelve
 *                                   su identificador público (nanoid 12).
 *   GET  /api/v1/messaging/users -> lista de usuarios con chat (excluye al
 *                                   propio; el cliente filtra localmente).
 *   GET  /api/v1/messaging/users/:public_id -> busca un usuario por su id
 *                                   público (devuelve usu_nombre).
 *   GET  /api/v1/messaging/conversations/:public_id/messages
 *                                   -> historial paginado de la conversación.
 *   POST /api/v1/messaging/conversations/:public_id/messages
 *                                   -> envía un mensaje y lo empuja por WS.
 *   POST /api/v1/messaging/conversations/:public_id/read
 *                                   -> marca mensajes como leídos.
 */
cws_router_t* messaging_routes(void);

#endif
