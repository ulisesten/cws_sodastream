#ifndef MESSAGING_DOMAIN_H
#define MESSAGING_DOMAIN_H

#include "cws/cws.h"
#include "sql_eject.h"

/*
 * Acceso a datos de mensajería 1:1 (procChatMensajesCons/Proc).
 * Debe llamarse messaging_domain_init() una vez tras cargar el .env.
 */

void messaging_domain_init(void);
void messaging_domain_shutdown(void);

/**
 * \brief Historial de la conversación (usu_id, usu_id_otro).
 *
 * Ejecuta procChatMensajesCons (tipoConsulta = 1). Devuelve los mensajes
 * más nuevos primero; filtra msg_id < msg_before si msg_before > 0 y limita
 * a `limit` (1..100; default 30). Filas: msg_id, msg_usu_emisor,
 * msg_usu_receptor, emisor_public, msg_tipo, msg_texto, msg_fecha,
 * msg_leido, msg_fecha_lectura, msg_id_respuesta.
 *
 * \return resultado malloc (liberar con sql_result_free), NULL en fallo.
 */
sql_result_t* messaging_domain_history(int64_t usu_id, int64_t usu_id_otro,
                                       int64_t msg_before, int limit);

/**
 * \brief Inserta un mensaje (procChatMensajesProc, tipoRegistro = 1).
 *
 * `tipo` es el TINYINT de msg_tipo: 0 = texto, 1 = sticker. Para sticker,
 * `sticker_id` (64 hex) y `texto` vacío. \return resultado malloc con
 * success/msg/error (+ msg_id, fecha, sticker_id y metadatos), NULL en fallo.
 */
sql_result_t* messaging_domain_insert(int64_t emisor, int64_t receptor,
                                      int tipo,
                                      const char* sticker_id,
                                      const char* texto, int64_t reply_to);

/**
 * \brief Marca como leídos los mensajes de `otro` hasta `hasta` (0 = todos).
 *        procChatMensajesProc, tipoRegistro = 2.
 */
sql_result_t* messaging_domain_mark_read(int64_t lector, int64_t otro,
                                         int64_t hasta);

/* ------------------------------------------------------------------ */
/* stickers                                                            */
/* ------------------------------------------------------------------ */

/** Directorio configurado (STICKERS_DIR; default "public/stickers"). */
const char* messaging_domain_stickers_dir(void);

/** Asegura que STICKERS_DIR exista y sea escribible (lo crea si falta).
 *  \return 1 si está listo; 0 si no (ruta inválida/sin permisos). */
int messaging_domain_stickers_ready(void);

/**
 * \brief Registra un sticker (procChatStickersProc, tipoRegistro = 1).
 *        Idempotente por `sti_hash` (64 hex). \return resultado con
 *        success/exists, NULL en fallo de BD.
 */
sql_result_t* messaging_domain_sticker_insert(const char* sti_hash,
                                              const char* mime,
                                              int64_t bytes, int width,
                                              int height, int animated,
                                              int64_t created_by);

/** Metadatos de un sticker por hash (Cons, tipoConsulta = 1). */
sql_result_t* messaging_domain_sticker_get(const char* sti_hash);

/** registered/contact_has para una lista CSV de ids (Cons, tipoConsulta=2). */
sql_result_t* messaging_domain_stickers_check(const char* ids_csv,
                                              int64_t contact_usu_id);

/** Reemplaza el inventario del usuario (Proc, tipoRegistro = 2). */
sql_result_t* messaging_domain_stickers_sync(int64_t usu_id,
                                             const char* ids_csv);

#endif
