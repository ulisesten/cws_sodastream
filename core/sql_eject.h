#ifndef SQL_EJECT_H
#define SQL_EJECT_H

#include <stddef.h>
#include <stdint.h>

#include "cws/cws.h"
#include "configuration.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SQL Eject — ejecutor de stored procedures de MS SQL Server (ODBC).
 *
 * Adaptación de la clase SqlEject de streaming_server/src/server/app/core/sql_eject.js.
 *
 *  - La configuración/credenciales se leen de core/configuration.h
 *    (DB_USER, DB_PASSWORD, DB_SERVER, DB_DATABASE, DB_ENCRYPT,
 *    DB_TRUST_CERTIFICATE, DB_PORT, DB_DRIVER).
 *  - Introspecciona los parámetros del SP (sys.parameters), construye una
 *    plantilla de valores por defecto (usuario_alta/mod, fecha_alta/mod,
 *    0, 1900-01-01 00:00:00, NULL), aplica los parámetros del llamador y
 *    ejecuta el SP.
 *  - Devuelve un result set tipado (filas/columnas), sin serialización JSON,
 *    para no sacrificar rendimiento. Hay un helper opcional para convertir a
 *    JSON cuando se necesita para una respuesta HTTP.
 */

/* --- fecha/hora ---------------------------------------------------------- */

typedef struct sql_datetime {
    int16_t year;
    int16_t month;
    int16_t day;
    int16_t hour;
    int16_t minute;
    int16_t second;
    int32_t nanosecond;
} sql_datetime_t;

/* --- parámetros de entrada ------------------------------------------------ */

typedef enum sql_param_type {
    SQL_PT_NULL = 0,     /* enlaza SQL NULL                    */
    SQL_PT_INT,          /* entero de 64 bits (int/bigint/...) */
    SQL_PT_DECIMAL,      /* double (decimal/numeric/float)     */
    SQL_PT_STRING,       /* const char* UTF-8 (varchar/nvarchar) */
    SQL_PT_DATETIME,     /* sql_datetime_t (datetime/date)     */
} sql_param_type_t;

typedef struct sql_param {
    const char*      name;   /* nombre del parámetro del SP, sin el '@' inicial */
    sql_param_type_t type;
    union {
        int64_t         as_int;
        double          as_decimal;
        const char*     as_string;
        sql_datetime_t  as_datetime;
    } val;
} sql_param_t;

/* --- result set tipado ---------------------------------------------------- */

typedef enum sql_column_type {
    SQL_COL_NULL = 0,
    SQL_COL_INT,        /* int / smallint / tinyint / bigint / bit */
    SQL_COL_DECIMAL,    /* decimal / numeric / float / real / money */
    SQL_COL_STRING,     /* (n)varchar / (n)char / text / binary(hex) */
    SQL_COL_DATETIME,   /* datetime / date / time */
} sql_column_type_t;

typedef struct sql_column {
    const char*        name;   /* propiedad del result */
    sql_column_type_t  type;
    size_t             size;   /* tamaño/precisión */
    int                scale;
} sql_column_t;

typedef struct sql_value {
    sql_column_type_t  type;
    int                is_null;
    union {
        int64_t         as_int;
        double          as_decimal;
        char*           as_string;   /* propiedad del result, NUL-terminated */
        sql_datetime_t  as_datetime;
    } val;
} sql_value_t;

typedef struct sql_row {
    size_t        ncols;
    sql_value_t*  cols;
} sql_row_t;

typedef struct sql_result {
    size_t         ncols;
    sql_column_t*  columns;
    size_t         nrows;
    sql_row_t*     rows;
} sql_result_t;

/* --- ejecutor ------------------------------------------------------------- */

typedef struct sql_eject sql_eject_t;

/* Construye el ejecutor a partir de la configuración central
 * (core/configuration.h). El config puede liberarse después; los valores se
 * copian. */
sql_eject_t* sql_eject_new(const app_config_t* cfg);
void         sql_eject_free(sql_eject_t* se);

/* Usuario que se estampa en usuario_alta/usuario_mod (default 1). */
void sql_eject_set_login_id(sql_eject_t* se, int64_t login_id);

/*
 * Ejecuta el stored procedure `sp_name` en la base `database`.
 * `params`/`nparams` pueden ser NULL/0 si el SP no recibe argumentos del
 * llamador. En éxito devuelve CWS_OK y llena `out` (liberar con
 * sql_result_free). Código de error negativo de cws en fallo.
 */
int sql_eject_store(sql_eject_t*       se,
                    const char*        sp_name,
                    const char*        database,
                    const sql_param_t* params,
                    size_t             nparams,
                    sql_result_t*      out);

/* Libera la memoria de un sql_result_t. */
void sql_result_free(sql_result_t* res);

/*
 * Serializa un sql_result_t a JSON (array de objetos con las columnas como
 * claves). El buffer es propiedad del llamador (free() con libc).
 * NULL si no hay memoria. Equivalente al recordset de mssql en Node.
 */
char* sql_result_to_json(const sql_result_t* res);

#ifdef __cplusplus
}
#endif

#endif /* SQL_EJECT_H */
