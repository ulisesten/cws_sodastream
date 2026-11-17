#define _POSIX_C_SOURCE 200809L

#include "telegram_calls.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define TC_MAX_CALLS   256
#define TC_RING_TTL    90
#define TC_ACTIVE_TTL  (6 * 3600)
#define TC_KEEP_ENDED  (3600)

static telegram_call_t g_calls[TC_MAX_CALLS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void copy_str(char* dst, size_t cap, const char* src) {
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void telegram_calls_init(void) {
    pthread_mutex_lock(&g_lock);
    memset(g_calls, 0, sizeof(g_calls));
    pthread_mutex_unlock(&g_lock);
}

static int find_call(const char* call_id) {
    if (!call_id || !*call_id) return -1;
    for (int i = 0; i < TC_MAX_CALLS; i++) {
        if (g_calls[i].call_id[0] && strcmp(g_calls[i].call_id, call_id) == 0)
            return i;
    }
    return -1;
}

static void cleanup_locked(time_t now) {
    for (int i = 0; i < TC_MAX_CALLS; i++) {
        telegram_call_t* c = &g_calls[i];
        if (!c->call_id[0]) continue;
        if (c->state == 0 && now - c->created > TC_RING_TTL) {
            c->state = 2;
            c->updated = now;
        } else if (c->state == 1 && now - c->updated > TC_ACTIVE_TTL) {
            c->state = 2;
            c->updated = now;
        } else if ((c->state == 2 || c->state == 3) &&
                   now - c->updated > TC_KEEP_ENDED) {
            memset(c, 0, sizeof(*c));
        }
    }
}

static int alloc_slot_locked(time_t now) {
    for (int i = 0; i < TC_MAX_CALLS; i++)
        if (!g_calls[i].call_id[0]) return i;
    int oldest = 0;
    for (int i = 1; i < TC_MAX_CALLS; i++) {
        if (g_calls[i].updated < g_calls[oldest].updated) oldest = i;
    }
    (void)now;
    return oldest;
}

int telegram_calls_upsert(const char* call_id, int64_t caller_uid,
                          int64_t callee_uid, const char* caller_public,
                          const char* callee_public, const char* caller_name,
                          const char* callee_name, const char* tipo,
                          const char* caller_ip, const char* caller_phone,
                          int e2ee_v, const char* e2ee_algs,
                          int e2ee_confirm) {
    if (!call_id || !*call_id || caller_uid == 0 || callee_uid == 0) return 0;
    time_t now = time(NULL);
    pthread_mutex_lock(&g_lock);
    cleanup_locked(now);
    int idx = find_call(call_id);
    int prev_state = idx >= 0 ? g_calls[idx].state : -1;
    if (idx < 0) {
        idx = alloc_slot_locked(now);
        memset(&g_calls[idx], 0, sizeof(g_calls[idx]));
        copy_str(g_calls[idx].call_id, sizeof(g_calls[idx].call_id), call_id);
        g_calls[idx].created = now;
    }
    telegram_call_t* c = &g_calls[idx];
    c->caller_uid = caller_uid;
    c->callee_uid = callee_uid;
    copy_str(c->caller_public, sizeof(c->caller_public), caller_public);
    copy_str(c->callee_public, sizeof(c->callee_public), callee_public);
    copy_str(c->caller_name, sizeof(c->caller_name), caller_name);
    copy_str(c->callee_name, sizeof(c->callee_name), callee_name);
    copy_str(c->tipo, sizeof(c->tipo), tipo);
    copy_str(c->caller_ip, sizeof(c->caller_ip), caller_ip);
    copy_str(c->caller_phone, sizeof(c->caller_phone), caller_phone);
    c->caller_e2ee_v = e2ee_v;
    c->caller_e2ee_confirm = e2ee_confirm;
    copy_str(c->caller_e2ee_algs, sizeof(c->caller_e2ee_algs), e2ee_algs);
    if (prev_state < 0 || prev_state == 2 || prev_state == 3)
        c->state = 0;
    c->updated = now;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int telegram_calls_get(const char* call_id, telegram_call_t* out) {
    if (!call_id || !out) return 0;
    time_t now = time(NULL);
    pthread_mutex_lock(&g_lock);
    cleanup_locked(now);
    int idx = find_call(call_id);
    if (idx >= 0) *out = g_calls[idx];
    pthread_mutex_unlock(&g_lock);
    return idx >= 0;
}

int telegram_calls_accept(const char* call_id, int64_t callee_uid,
                          const char* callee_ip, const char* callee_phone,
                          int e2ee_v, const char* e2ee_algs,
                          int e2ee_confirm, telegram_call_t* out) {
    if (!call_id || callee_uid == 0) return 0;
    time_t now = time(NULL);
    pthread_mutex_lock(&g_lock);
    cleanup_locked(now);
    int idx = find_call(call_id);
    int ok = 0;
    if (idx >= 0 && g_calls[idx].callee_uid == callee_uid &&
        g_calls[idx].state == 0) {
        g_calls[idx].state = 1;
        copy_str(g_calls[idx].callee_ip, sizeof(g_calls[idx].callee_ip),
                 callee_ip);
        copy_str(g_calls[idx].callee_phone, sizeof(g_calls[idx].callee_phone),
                 callee_phone);
        g_calls[idx].callee_e2ee_v = e2ee_v;
        g_calls[idx].callee_e2ee_confirm = e2ee_confirm;
        copy_str(g_calls[idx].callee_e2ee_algs,
                 sizeof(g_calls[idx].callee_e2ee_algs), e2ee_algs);
        g_calls[idx].updated = now;
        if (out) *out = g_calls[idx];
        ok = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int telegram_calls_reject(const char* call_id, int64_t callee_uid,
                          telegram_call_t* out) {
    if (!call_id || callee_uid == 0) return 0;
    time_t now = time(NULL);
    pthread_mutex_lock(&g_lock);
    cleanup_locked(now);
    int idx = find_call(call_id);
    int ok = 0;
    if (idx >= 0 && g_calls[idx].callee_uid == callee_uid &&
        g_calls[idx].state == 0) {
        g_calls[idx].state = 3;
        g_calls[idx].updated = now;
        if (out) *out = g_calls[idx];
        ok = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int telegram_calls_end(const char* call_id, int64_t uid,
                       telegram_call_t* out) {
    if (!call_id || uid == 0) return 0;
    time_t now = time(NULL);
    pthread_mutex_lock(&g_lock);
    cleanup_locked(now);
    int idx = find_call(call_id);
    int ok = 0;
    if (idx >= 0 && (g_calls[idx].caller_uid == uid ||
                     g_calls[idx].callee_uid == uid) &&
        (g_calls[idx].state == 1 || g_calls[idx].state == 0)) {
        g_calls[idx].state = 2;
        g_calls[idx].updated = now;
        if (out) *out = g_calls[idx];
        ok = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int telegram_calls_pending(int64_t uid, telegram_call_t* out, size_t max) {
    if (!out || max == 0 || uid == 0) return 0;
    time_t now = time(NULL);
    int n = 0;
    pthread_mutex_lock(&g_lock);
    cleanup_locked(now);
    for (int i = 0; i < TC_MAX_CALLS && (size_t)n < max; i++) {
        telegram_call_t* c = &g_calls[i];
        if (!c->call_id[0]) continue;
        if (c->state > 1) continue;
        if (c->caller_uid != uid && c->callee_uid != uid) continue;
        out[n++] = *c;
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}
