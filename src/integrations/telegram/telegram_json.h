#ifndef TELEGRAM_JSON_H
#define TELEGRAM_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TJ_NULL = 0,
    TJ_BOOL,
    TJ_NUM,
    TJ_STR,
    TJ_ARR,
    TJ_OBJ
} tj_type_t;

typedef struct tj_value tj_value_t;

/* Parse a complete JSON document. Returns NULL on invalid input. */
tj_value_t* tj_parse(const char* text, size_t len);
void        tj_free(tj_value_t* v);

tj_value_t* tj_obj_get(const tj_value_t* obj, const char* key);
tj_value_t* tj_arr_get(const tj_value_t* arr, size_t idx);
size_t      tj_arr_len(const tj_value_t* arr);

const char* tj_str(const tj_value_t* v);
int64_t     tj_int(const tj_value_t* v);
int         tj_bool(const tj_value_t* v);
tj_type_t   tj_type(const tj_value_t* v);

#ifdef __cplusplus
}
#endif

#endif /* TELEGRAM_JSON_H */
