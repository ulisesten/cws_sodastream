#ifndef FCM_H
#define FCM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 enviado/aceptado en dry-run; 0 error; -1 token inválido. */
#define FCM_SENT 1
#define FCM_ERROR 0
#define FCM_INVALID_TOKEN (-1)

int  fcm_init(void);
void fcm_shutdown(void);
int  fcm_enabled(void);

/*
 * payload_json debe ser un objeto JSON con valores string (data-only).
 * priority: "high" o "normal"; ttl en segundos; collapse puede ser NULL.
 */
int fcm_send(const char* token, const char* payload_json,
             const char* priority, int ttl, const char* collapse,
             char* err, size_t errcap);

#ifdef __cplusplus
}
#endif

#endif /* FCM_H */
