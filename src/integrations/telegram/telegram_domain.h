#ifndef TELEGRAM_DOMAIN_H
#define TELEGRAM_DOMAIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TELEGRAM_TG_USER_MAX 64
#define TELEGRAM_TG_NAME_MAX 128

typedef struct telegram_link {
    int64_t usu_id;
    int64_t chat_id;
    char    username[TELEGRAM_TG_USER_MAX];
    char    name[TELEGRAM_TG_NAME_MAX];
    int     active;
} telegram_link_t;

void telegram_domain_init(void);
void telegram_domain_shutdown(void);

/* Crea un código de vinculación de un solo uso. */
int telegram_domain_create_code(int64_t usu_id, const char* code, int ttl_min);

/* Consume el código y vincula el chat. Devuelve 1 y el usu_id interno. */
int telegram_domain_consume_code(const char* code, int64_t chat_id,
                                 const char* username, const char* name,
                                 int64_t* usu_id_out);

/* Vínculo activo por usuario o por chat. Devuelve 1 si existe. */
int telegram_domain_get_link(int64_t usu_id, telegram_link_t* out);
int telegram_domain_get_link_by_chat(int64_t chat_id, telegram_link_t* out);

/* usu_id_public y nombre de un usuario (para el evento call.incoming). */
int telegram_domain_user_public(int64_t usu_id, char* public_out,
                                size_t public_cap, char* name_out,
                                size_t name_cap);

int telegram_domain_unlink_user(int64_t usu_id);
int telegram_domain_unlink_chat(int64_t chat_id);

/* Mapeo externo: tipo 1 teléfono, 2 IP, 3 ID externo. */
int telegram_domain_external_link(int64_t usu_id, int type, const char* value);
int telegram_domain_external_unlink(int64_t usu_id, int type,
                                    const char* value);
int telegram_domain_external_lookup(int type, const char* value,
                                    int64_t* usu_id_out,
                                    char* public_out, size_t public_cap,
                                    char* name_out, size_t name_cap);

#ifdef __cplusplus
}
#endif

#endif /* TELEGRAM_DOMAIN_H */
