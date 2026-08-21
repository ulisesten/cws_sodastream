#ifndef VIDEOS_DOMAIN_H
#define VIDEOS_DOMAIN_H

#include "cws/cws.h"

/* Inicializa el acceso a datos del módulo de videos (config + sql_eject).
 * Debe llamarse una vez tras cargar el .env del app. */
void videos_domain_init(const cws_app_t* app);

void get_all_videos(cws_request_t* req, cws_response_t* res);
void get_video_by_id(cws_request_t* req, cws_response_t* res);

#endif
