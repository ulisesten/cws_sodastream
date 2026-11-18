#ifndef PUSH_H
#define PUSH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  push_init(void);
void push_shutdown(void);
int  push_enabled(void);

/* Encola un evento para todos los dispositivos activos del usuario.
 * Devuelve cuántos dispositivos se encolaron. */
int push_notify_user(int64_t usu_id, const char* event_key,
                     const char* type, const char* payload_json,
                     const char* collapse_key, int delay_s);

int push_notify_call(int64_t usu_id, const char* call_id,
                     const char* from_name, const char* from_public,
                     const char* tipo, int e2ee,
                     const char* event_key, const char* type);

int push_notify_call_event(int64_t usu_id, const char* call_id,
                           const char* type);

int push_notify_message(int64_t usu_id, const char* chat_public,
                        int64_t msg_id, const char* from_name,
                        const char* from_public, const char* body,
                        int silent);

#ifdef __cplusplus
}
#endif

#endif /* PUSH_H */
