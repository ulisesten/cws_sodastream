#ifndef RELEASES_ROUTES_H
#define RELEASES_ROUTES_H

#include "cws/cws.h"

/* Inicializa el acceso a datos del módulo de releases (config + sql_eject).
 * Debe llamarse una vez tras cargar el .env del app. */
void releases_routes_init(void);

/* Router del módulo releases, montado en /api/v1/app:
 *   GET /api/v1/app/releases -> get_all_releases */
cws_router_t* releases_routes(void);

#endif
