#ifndef TELEGRAM_CALLS_H
#define TELEGRAM_CALLS_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TC_CALL_ID_MAX 65
#define TC_IP_MAX      64
#define TC_PHONE_MAX   32
#define TC_NAME_MAX    128
#define TC_PUBLIC_MAX  16
#define TC_TIPO_MAX    16

typedef struct telegram_call {
    char     call_id[TC_CALL_ID_MAX];
    int64_t  caller_uid;
    int64_t  callee_uid;
    char     caller_public[TC_PUBLIC_MAX];
    char     callee_public[TC_PUBLIC_MAX];
    char     caller_name[TC_NAME_MAX];
    char     callee_name[TC_NAME_MAX];
    char     tipo[TC_TIPO_MAX];
    char     caller_ip[TC_IP_MAX];
    char     callee_ip[TC_IP_MAX];
    char     caller_phone[TC_PHONE_MAX];
    char     callee_phone[TC_PHONE_MAX];
    int      state; /* 0 ringing, 1 accepted, 2 ended, 3 rejected */
    time_t   created;
    time_t   updated;
} telegram_call_t;

void telegram_calls_init(void);

int telegram_calls_upsert(const char* call_id, int64_t caller_uid,
                          int64_t callee_uid, const char* caller_public,
                          const char* callee_public, const char* caller_name,
                          const char* callee_name, const char* tipo,
                          const char* caller_ip, const char* caller_phone);

int telegram_calls_get(const char* call_id, telegram_call_t* out);

int telegram_calls_accept(const char* call_id, int64_t callee_uid,
                          const char* callee_ip, const char* callee_phone,
                          telegram_call_t* out);

int telegram_calls_reject(const char* call_id, int64_t callee_uid,
                          telegram_call_t* out);

int telegram_calls_end(const char* call_id, int64_t uid,
                       telegram_call_t* out);

int telegram_calls_pending(int64_t uid, telegram_call_t* out, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* TELEGRAM_CALLS_H */
