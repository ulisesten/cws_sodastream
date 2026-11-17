#ifndef CALLS_IDENTITY_DOMAIN_H
#define CALLS_IDENTITY_DOMAIN_H

#include <stdint.h>

#include "sql_eject.h"

#ifdef __cplusplus
extern "C" {
#endif

void calls_identity_domain_init(void);
void calls_identity_domain_shutdown(void);

int calls_identity_upsert(int64_t usu_id, const char* device_id,
                          const char* public_key);

sql_result_t* calls_identity_list(int64_t usu_id);

#ifdef __cplusplus
}
#endif

#endif /* CALLS_IDENTITY_DOMAIN_H */
