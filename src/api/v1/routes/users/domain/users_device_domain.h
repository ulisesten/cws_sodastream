#ifndef USERS_DEVICE_DOMAIN_H
#define USERS_DEVICE_DOMAIN_H

#include <stdint.h>

#include "sql_eject.h"

#ifdef __cplusplus
extern "C" {
#endif

void users_device_domain_init(void);
void users_device_domain_shutdown(void);

int users_device_set(int64_t usu_id, const char* device_id);
int users_device_clear(int64_t usu_id);
sql_result_t* users_device_get(int64_t usu_id);

#ifdef __cplusplus
}
#endif

#endif /* USERS_DEVICE_DOMAIN_H */
