/*
 * hash_table.c — FNV-1a 32 bits (ver core/hash_table.h).
 */

#include "hash_table.h"

uint32_t hash_string(const char* s) {
    if (!s) return 0;
    uint32_t h = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        h ^= (uint32_t)*p;
        h *= 16777619u;
    }
    return h;
}
