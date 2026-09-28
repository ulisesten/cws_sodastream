#ifndef VIDEOS_DOMAIN_H
#define VIDEOS_DOMAIN_H

#include "cws/cws.h"

/* Inicializa el acceso a datos del módulo de videos (config + sql_eject).
 * Debe llamarse una vez tras cargar el .env del app. */
void videos_domain_init(void);

void get_all_videos(cws_request_t* req, cws_response_t* res);
void get_popular_videos(cws_request_t* req, cws_response_t* res);
void get_video_by_id(cws_request_t* req, cws_response_t* res);

/*
 * GET /api/v1/videos/thumbnails/:name — sirve la imagen de la miniatura
 * (name = "<thu_id_public>.<ext>" o "<thu_id_public>"). Resuelve el archivo
 * en API_NAS con fallback de extensiones y responde sendfile; 404 JSON si
 * no existe.
 */
void get_thumbnail(cws_request_t* req, cws_response_t* res);

/*
 * PUT /api/v1/videos/:id/views — incrementa el contador de vistas
 * (procCatVideosProc, tipoRegistro = "CAT_VIDEOS_VIEW"). Responde el DTO
 * general (msg/success/error).
 */
void insert_view(cws_request_t* req, cws_response_t* res);

/*
 * GET /api/v1/videos/:id/series/relacionados — videos de la serie del video
 * (procCatVideosCons, tipoConsulta = CAT_SERIES_VIDEOS_CONS).
 */
void get_series_videos(cws_request_t* req, cws_response_t* res);

/*
 * POST /api/v1/videos/external — registra un video de fuente externa (URL
 * m3u8 + miniatura existente, sin subida física) a nombre del usuario
 * autenticado (ruta protegida: cookie + CSRF + IP). Genera vid_id_public
 * (nanoid de PUBLIC_ID_LENGTH) y ejecuta procCatVideosProc con
 * tipoRegistro = "CAT_VIDEOS_EXTERNAL_INS". Responde el DTO
 * subir_video_response.
 */
void insert_external_video(cws_request_t* req, cws_response_t* res);

#endif
