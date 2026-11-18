#ifndef USERS_DEVICE_DOMAIN_H
#define USERS_DEVICE_DOMAIN_H

#include <stdint.h>

#include "sql_eject.h"

#ifdef __cplusplus
extern "C" {
#endif

void users_device_domain_init(void);
void users_device_domain_shutdown(void);

/* Upsert de un dispositivo. Devuelve el result set (liberar con
 * sql_result_free); comprobar con users_device_ok(). */
sql_result_t* users_devices_upsert(int64_t usu_id, const char* device_id,
                                   int plataforma, const char* nombre,
                                   const char* push_token);

/* active_only=1 lista activos; 0 lista todos. */
sql_result_t* users_devices_list(int64_t usu_id, int active_only);
sql_result_t* users_devices_get(int64_t usu_id, const char* device_id);

/* Activos con push token: usd_device_id, usd_push_token. */
sql_result_t* users_devices_push_tokens(int64_t usu_id);

int users_devices_deactivate(int64_t usu_id, const char* device_id);
int users_devices_delete(int64_t usu_id, const char* device_id);
int users_devices_logout_all(int64_t usu_id);

/* 1 si el result set trae success='true'. */
int users_device_ok(const sql_result_t* out);

#ifdef __cplusplus
}
#endif

#endif /* USERS_DEVICE_DOMAIN_H */
