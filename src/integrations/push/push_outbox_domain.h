#ifndef PUSH_OUTBOX_DOMAIN_H
#define PUSH_OUTBOX_DOMAIN_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUSH_TOKEN_MAX   512
#define PUSH_DEVICE_MAX  255
#define PUSH_TYPE_MAX    64
#define PUSH_PAYLOAD_MAX 4000
#define PUSH_COLLAPSE_MAX 128
#define PUSH_PRIORITY_MAX 16

typedef struct push_outbox_item {
    int64_t id;
    int64_t usu_id;
    char    device_id[PUSH_DEVICE_MAX + 1];
    char    token[PUSH_TOKEN_MAX + 1];
    char    type[PUSH_TYPE_MAX + 1];
    char    payload[PUSH_PAYLOAD_MAX + 1];
    char    collapse[PUSH_COLLAPSE_MAX + 1];
    char    priority[PUSH_PRIORITY_MAX + 1];
    int     ttl;
    int     attempts;
} push_outbox_item_t;

void push_outbox_domain_init(void);
void push_outbox_domain_shutdown(void);

int push_outbox_enqueue(int64_t usu_id, const char* device_id,
                        const char* token, const char* type,
                        const char* payload, const char* collapse,
                        const char* priority, int ttl, int delay_s,
                        const char* event_key);

/* Devuelve cuántos items reclamó. */
int push_outbox_claim(int limit, push_outbox_item_t* out, size_t max);

/* status: 2 sent, 3 retry, 4 dead. */
int push_outbox_mark(int64_t id, int status, const char* error, int retry_s);
int push_outbox_cancel(int64_t usu_id, const char* event_key,
                       const char* device_id);

#ifdef __cplusplus
}
#endif

#endif /* PUSH_OUTBOX_DOMAIN_H */
