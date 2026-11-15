#ifndef TELEGRAM_H
#define TELEGRAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Inicia el bot (long polling) si TELEGRAM_BOT_TOKEN está configurado.
 * Devuelve 1 si quedó activo, 0 si está deshabilitado. */
int  telegram_init(void);
void telegram_shutdown(void);

int         telegram_enabled(void);
const char* telegram_bot_username(void);
int         telegram_link_ttl_min(void);
int         telegram_notify_always(void);

/* Envía un mensaje de texto a un chat. Devuelve 1 si Telegram respondió ok. */
int telegram_send_message(int64_t chat_id, const char* text);

/* Envía una notificación de llamada al usuario si tiene Telegram vinculado.
 * Devuelve 1 si se envió, 0 si no hay vínculo o falló. */
int telegram_notify_call(int64_t usu_id, const char* from_name,
                         const char* call_id, const char* tipo);

/* Emite `call.incoming` por WSS y, si el usuario no está online (o
 * TELEGRAM_NOTIFY_ALWAYS=1), manda el aviso por Telegram.
 * Devuelve 1 si se envió por Telegram; deja el estado WSS en ws_online. */
int telegram_notify_bridge(int64_t usu_id, const char* from_public,
                           const char* from_name, const char* from_ip,
                           const char* call_id, const char* tipo,
                           int* ws_online);

#ifdef __cplusplus
}
#endif

#endif /* TELEGRAM_H */
