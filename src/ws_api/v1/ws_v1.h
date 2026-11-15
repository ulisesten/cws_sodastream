#ifndef WS_API_V1_WS_V1_H
#define WS_API_V1_WS_V1_H

#include "cws/cws.h"

/*
 * WebSocket API v1 (src/ws_api/v1).
 *
 * Inicializa el hub WebSocket de la lib cws (handshake GOST/Streebog) y monta
 * el endpoint de prueba en WS_V1_PATH ("/ws/v1"). Debe llamarse una sola vez,
 * después de cargar el .env y antes de cws_app_run().
 *
 * Devuelve CWS_OK o un error de cws (CWS_ERR_*).
 */
int ws_v1_init(cws_app_t* app);

/* Hub WebSocket inicializado (NULL si ws_v1_init no se llamó o falló). */
cws_ws_t* ws_v1_hub(void);

/* Ruta del endpoint ("/ws/v1"). */
const char* ws_v1_path(void);

/*
 * Emite un evento a todos los sockets del usuario (sala interna
 * user:<usu_id>). Pensado para push del chat 1:1 (`dm`). No expone usu_id
 * en el payload; el llamador arma el JSON público.
 *
 * \return CWS_OK o error de cws (CWS_ERR_INVALID si no hay hub).
 */
int ws_v1_emit_to_user(int64_t usu_id, const char* event,
                       const void* payload, size_t len);

#endif
