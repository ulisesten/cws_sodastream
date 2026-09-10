/*
 * sql_eject.c — ejecutor de stored procedures de MS SQL Server vía ODBC.
 *
 * Espejo en C de streaming_server/src/server/app/core/sql_eject.js (SqlEject).
 * Usa unixODBC + Microsoft ODBC Driver 17 for SQL Server.
 */

#define _POSIX_C_SOURCE 200809L

#include "sql_eject.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <sql.h>
#include <sqlext.h>

/* Algunas builds de unixODBC omiten estos símbolos del estándar ODBC.
 * Nota: SQL_MONEY(1)/SQL_SMALLMONEY(2) colisionan con SQL_CHAR/SQL_NUMERIC
 * y no se usan aquí. */
#ifndef SQL_WCHAR
#define SQL_WCHAR (-8)
#endif
#ifndef SQL_WVARCHAR
#define SQL_WVARCHAR (-9)
#endif
#ifndef SQL_WLONGVARCHAR
#define SQL_WLONGVARCHAR (-10)
#endif
#ifndef SQL_C_WCHAR
#define SQL_C_WCHAR SQL_WCHAR
#endif

#define SQL_EJECT_ROW_BATCH 32
#define SQL_EJECT_MAX_COL_NAME 256

/* ------------------------------------------------------------------------- */
/* tipos internos                                                            */
/* ------------------------------------------------------------------------- */

typedef enum value_class {
    VC_NULL = 0,
    VC_INT,
    VC_DECIMAL,
    VC_STRING_ANSI,
    VC_STRING_UTF16,
    VC_DATETIME,
} value_class_t;

struct sql_eject {
    char*   driver;
    char*   server;
    char*   user;
    char*   password;
    char*   database;
    char*   port;
    int     encrypt;
    int     trust_cert;
    int64_t login_id;
};

typedef struct sp_param {
    char* name;       /* sin '@' */
    char* data_type;  /* nombre de sys.types en minúsculas */
    int   precision;
    int   scale;
    int   max_length;
} sp_param_t;

typedef struct bind_param {
    char*        name;
    SQLSMALLINT  sql_type;
    SQLULEN      column_size;
    SQLSMALLINT  decimal_digits;
    int          max_length;
    int          is_unicode;
    value_class_t value_kind;
    int          is_null;

    /* info de enlace */
    SQLSMALLINT  c_type;
    SQLPOINTER   ptr;
    SQLLEN       ind;
    SQLLEN       buf_len;

    /* almacenamiento del valor */
    int64_t              i;
    double               d;
    char*                str;      /* utf-8 (para tipos ANSI) */
    uint16_t*            ws;       /* utf-16 (para tipos Unicode) */
    SQLLEN               ws_bytes;
    SQL_TIMESTAMP_STRUCT ts;
} bind_param_t;

typedef struct out_col {
    char*          name;
    sql_column_type_t type;
    size_t         size;
    int            scale;
    int            is_unicode;
    int            is_binary;
    SQLSMALLINT    c_type;
    SQLPOINTER     buf;
    SQLLEN         buf_alloc;
    SQLLEN         ind;
} out_col_t;

/* ------------------------------------------------------------------------- */
/* utilidades de log                                                         */
/* ------------------------------------------------------------------------- */

static void log_odbc_error(SQLSMALLINT handle_type, SQLHANDLE handle,
                           const char* what) {
    SQLCHAR sqlstate[6];
    SQLCHAR msg[SQL_MAX_MESSAGE_LENGTH];
    SQLINTEGER native;
    SQLSMALLINT len;
    SQLRETURN r;
    int i = 1;
    while ((r = SQLGetDiagRec(handle_type, handle, (SQLSMALLINT)i, sqlstate,
                              &native, msg, sizeof(msg), &len)) == SQL_SUCCESS ||
           r == SQL_SUCCESS_WITH_INFO) {
        cws_log_error("%s: SQLSTATE=%s native=%ld %s", what, sqlstate,
                      (long)native, msg);
        i++;
    }
}

/* ------------------------------------------------------------------------- */
/* conversión UTF-8 <-> UTF-16                                               */
/* ------------------------------------------------------------------------- */

static size_t utf8_to_utf16(const char* s, uint16_t* out, size_t cap) {
    size_t in_len = strlen(s);
    size_t i = 0, o = 0;
    while (i < in_len) {
        unsigned char c = (unsigned char)s[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) {
            cp = c;
            n = 1;
        } else if ((c >> 5) == 0x6 && i + 1 < in_len) {
            cp = c & 0x1F;
            n = 2;
        } else if ((c >> 4) == 0xE && i + 2 < in_len) {
            cp = c & 0x0F;
            n = 3;
        } else if ((c >> 3) == 0x1E && i + 3 < in_len) {
            cp = c & 0x07;
            n = 4;
        } else {
            i++;
            continue;
        }
        for (size_t k = 1; k < n; k++)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += n;
        if (cp > 0xFFFF) {
            cp -= 0x10000;
            if (out && o + 2 < cap) {
                out[o++] = (uint16_t)(0xD800 + (cp >> 10));
                out[o++] = (uint16_t)(0xDC00 + (cp & 0x3FF));
            } else {
                o += 2;
            }
        } else if (out && o < cap) {
            out[o++] = (uint16_t)cp;
        } else {
            o++;
        }
    }
    return o;
}

static char* utf16_to_utf8(const uint16_t* u, size_t len) {
    size_t bytes = 0;
    for (size_t i = 0; i < len; i++) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len &&
            u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80)
            bytes += 1;
        else if (cp < 0x800)
            bytes += 2;
        else if (cp < 0x10000)
            bytes += 3;
        else
            bytes += 4;
    }
    char* out = (char*)malloc(bytes + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        uint32_t cp = u[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len &&
            u[i + 1] >= 0xDC00 && u[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        }
        if (cp < 0x80) {
            out[o++] = (char)cp;
        } else if (cp < 0x800) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    out[o] = '\0';
    return out;
}

static char* hex_encode(const unsigned char* data, size_t n) {
    static const char digits[] = "0123456789abcdef";
    char* s = (char*)malloc(n * 2 + 1);
    if (!s) return NULL;
    for (size_t i = 0; i < n; i++) {
        s[i * 2] = digits[data[i] >> 4];
        s[i * 2 + 1] = digits[data[i] & 0x0F];
    }
    s[n * 2] = '\0';
    return s;
}

/* ------------------------------------------------------------------------- */
/* fecha/hora                                                                */
/* ------------------------------------------------------------------------- */

static void datetime_now(SQL_TIMESTAMP_STRUCT* ts) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    ts->year = (SQLSMALLINT)(tmv.tm_year + 1900);
    ts->month = (SQLUSMALLINT)(tmv.tm_mon + 1);
    ts->day = (SQLUSMALLINT)tmv.tm_mday;
    ts->hour = (SQLUSMALLINT)tmv.tm_hour;
    ts->minute = (SQLUSMALLINT)tmv.tm_min;
    ts->second = (SQLUSMALLINT)tmv.tm_sec;
    ts->fraction = 0;
}

static void datetime_to_ts(const sql_datetime_t* dt, SQL_TIMESTAMP_STRUCT* ts) {
    ts->year = dt->year;
    ts->month = (SQLUSMALLINT)dt->month;
    ts->day = (SQLUSMALLINT)dt->day;
    ts->hour = (SQLUSMALLINT)dt->hour;
    ts->minute = (SQLUSMALLINT)dt->minute;
    ts->second = (SQLUSMALLINT)dt->second;
    /* fraction está en nanosegundos (10^-9 s) en ODBC 3.0 */
    ts->fraction = (SQLUINTEGER)dt->nanosecond;
}

static void ts_to_datetime(const SQL_TIMESTAMP_STRUCT* ts, sql_datetime_t* dt) {
    dt->year = ts->year;
    dt->month = (int16_t)ts->month;
    dt->day = (int16_t)ts->day;
    dt->hour = (int16_t)ts->hour;
    dt->minute = (int16_t)ts->minute;
    dt->second = (int16_t)ts->second;
    dt->nanosecond = (int32_t)ts->fraction;
}

/* ------------------------------------------------------------------------- */
/* mapeo de tipos SQL                                                         */
/* ------------------------------------------------------------------------- */

static SQLSMALLINT odbc_type_for(const char* t, int* is_unicode, int* is_int,
                                 int* is_datetime) {
    *is_unicode = 0;
    *is_int = 0;
    *is_datetime = 0;
    if (!strcmp(t, "int")) { *is_int = 1; return SQL_INTEGER; }
    if (!strcmp(t, "bigint")) { *is_int = 1; return SQL_BIGINT; }
    if (!strcmp(t, "smallint")) { *is_int = 1; return SQL_SMALLINT; }
    if (!strcmp(t, "tinyint")) { *is_int = 1; return SQL_TINYINT; }
    if (!strcmp(t, "bit")) { *is_int = 1; return SQL_BIT; }
    if (!strcmp(t, "decimal") || !strcmp(t, "numeric")) return SQL_NUMERIC;
    if (!strcmp(t, "float")) return SQL_FLOAT;
    if (!strcmp(t, "real")) return SQL_REAL;
    if (!strcmp(t, "datetime") || !strcmp(t, "datetime2") ||
        !strcmp(t, "smalldatetime")) {
        *is_datetime = 1;
        return SQL_TYPE_TIMESTAMP;
    }
    if (!strcmp(t, "date")) { *is_datetime = 1; return SQL_TYPE_DATE; }
    if (!strcmp(t, "time")) { *is_datetime = 1; return SQL_TYPE_TIME; }
    if (!strcmp(t, "char")) return SQL_CHAR;
    if (!strcmp(t, "varchar")) return SQL_VARCHAR;
    if (!strcmp(t, "nchar")) { *is_unicode = 1; return SQL_WCHAR; }
    if (!strcmp(t, "nvarchar")) { *is_unicode = 1; return SQL_WVARCHAR; }
    if (!strcmp(t, "text")) return SQL_LONGVARCHAR;
    if (!strcmp(t, "ntext")) { *is_unicode = 1; return SQL_WLONGVARCHAR; }
    if (!strcmp(t, "binary") || !strcmp(t, "varbinary")) return SQL_VARBINARY;
    if (!strcmp(t, "uniqueidentifier")) return SQL_GUID;
    if (!strcmp(t, "xml")) { *is_unicode = 1; return SQL_WLONGVARCHAR; }
    return SQL_VARCHAR;
}

static value_class_t value_class_of(SQLSMALLINT sql_type, int is_unicode) {
    switch (sql_type) {
        case SQL_INTEGER:
        case SQL_BIGINT:
        case SQL_SMALLINT:
        case SQL_TINYINT:
        case SQL_BIT:
            return VC_INT;
        case SQL_NUMERIC:
        case SQL_DECIMAL:
        case SQL_FLOAT:
        case SQL_REAL:
            return VC_DECIMAL;
        case SQL_TYPE_TIMESTAMP:
        case SQL_TYPE_DATE:
        case SQL_TYPE_TIME:
            return VC_DATETIME;
        default:
            return is_unicode ? VC_STRING_UTF16 : VC_STRING_ANSI;
    }
}

static sql_column_type_t out_type_of(SQLSMALLINT sql_type, int* is_unicode,
                                     int* is_binary) {
    *is_unicode = 0;
    *is_binary = 0;
    switch (sql_type) {
        case SQL_INTEGER:
        case SQL_SMALLINT:
        case SQL_TINYINT:
        case SQL_BIGINT:
        case SQL_BIT:
            return SQL_COL_INT;
        case SQL_NUMERIC:
        case SQL_DECIMAL:
        case SQL_FLOAT:
        case SQL_REAL:
        case SQL_DOUBLE:
            return SQL_COL_DECIMAL;
        case SQL_TYPE_TIMESTAMP:
        case SQL_TYPE_DATE:
        case SQL_TYPE_TIME:
            return SQL_COL_DATETIME;
        case SQL_WCHAR:
        case SQL_WVARCHAR:
        case SQL_WLONGVARCHAR:
            *is_unicode = 1;
            return SQL_COL_STRING;
        case SQL_BINARY:
        case SQL_VARBINARY:
        case SQL_LONGVARBINARY:
            *is_binary = 1;
            return SQL_COL_STRING;
        default:
            return SQL_COL_STRING;
    }
}

/* ------------------------------------------------------------------------- */
/* conexión                                                                  */
/* ------------------------------------------------------------------------- */

static int se_connect(sql_eject_t* se, const char* database, SQLHENV* phenv,
                      SQLHDBC* phdbc) {
    SQLRETURN r;
    SQLHENV env = SQL_NULL_HANDLE;
    SQLHDBC dbc = SQL_NULL_HANDLE;

    r = SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
    if (!SQL_SUCCEEDED(r)) {
        cws_log_error("sql_eject: SQLAllocHandle(ENV) falló (%d)", (int)r);
        return CWS_ERR_GENERIC;
    }
    SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, (SQLPOINTER)SQL_OV_ODBC3, 0);

    r = SQLAllocHandle(SQL_HANDLE_DBC, env, &dbc);
    if (!SQL_SUCCEEDED(r)) {
        log_odbc_error(SQL_HANDLE_ENV, env, "SQLAllocHandle(DBC)");
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return CWS_ERR_GENERIC;
    }

    char conn[2048];
    if (se->port && *se->port && strcmp(se->port, "1433") != 0)
        snprintf(conn, sizeof(conn),
                 "DRIVER={%s};SERVER=%s,%s;DATABASE=%s;UID=%s;PWD=%s;Encrypt=%s;TrustServerCertificate=%s;",
                 se->driver, se->server, se->port, database, se->user,
                 se->password, se->encrypt ? "yes" : "no",
                 se->trust_cert ? "yes" : "no");
    else
        snprintf(conn, sizeof(conn),
                 "DRIVER={%s};SERVER=%s;DATABASE=%s;UID=%s;PWD=%s;Encrypt=%s;TrustServerCertificate=%s;",
                 se->driver, se->server, database, se->user, se->password,
                 se->encrypt ? "yes" : "no", se->trust_cert ? "yes" : "no");

    r = SQLDriverConnect(dbc, NULL, (SQLCHAR*)conn, SQL_NTS, NULL, 0, NULL,
                         SQL_DRIVER_NOPROMPT);
    if (!SQL_SUCCEEDED(r)) {
        log_odbc_error(SQL_HANDLE_DBC, dbc, "SQLDriverConnect");
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
        SQLFreeHandle(SQL_HANDLE_ENV, env);
        return CWS_ERR_IO;
    }

    *phenv = env;
    *phdbc = dbc;
    return CWS_OK;
}

/* ------------------------------------------------------------------------- */
/* introspección de parámetros del SP                                         */
/* ------------------------------------------------------------------------- */

static int fetch_sp_params(SQLHDBC dbc, const char* sp_name, sp_param_t** out,
                           size_t* nout) {
    SQLRETURN r;
    SQLHSTMT stmt = SQL_NULL_HANDLE;
    SQLLEN ind_sp = SQL_NTS;
    *out = NULL;
    *nout = 0;

    r = SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
    if (!SQL_SUCCEEDED(r)) {
        log_odbc_error(SQL_HANDLE_DBC, dbc, "SQLAllocHandle(STMT)");
        return CWS_ERR_GENERIC;
    }

    const char* query =
        "SELECT p.name AS PARAMETER_NAME, t.name AS DATA_TYPE, "
        "p.precision AS NUMERIC_PRECISION, p.scale AS NUMERIC_SCALE, "
        "p.max_length AS CHARACTER_MAXIMUM_LENGTH "
        "FROM sys.parameters p "
        "JOIN sys.types t "
        "ON p.system_type_id = t.system_type_id "
        "AND p.user_type_id = t.user_type_id "
        "WHERE object_id = OBJECT_ID(?) "
        "ORDER BY parameter_id;";

    r = SQLPrepare(stmt, (SQLCHAR*)query, SQL_NTS);
    if (!SQL_SUCCEEDED(r)) {
        log_odbc_error(SQL_HANDLE_STMT, stmt, "SQLPrepare(schema)");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return CWS_ERR_GENERIC;
    }
    r = SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_WVARCHAR,
                         256, 0, (SQLPOINTER)sp_name, 0, &ind_sp);
    if (!SQL_SUCCEEDED(r)) {
        log_odbc_error(SQL_HANDLE_STMT, stmt, "SQLBindParameter(schema)");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return CWS_ERR_GENERIC;
    }
    r = SQLExecute(stmt);
    if (!SQL_SUCCEEDED(r)) {
        log_odbc_error(SQL_HANDLE_STMT, stmt, "SQLExecute(schema)");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return CWS_ERR_GENERIC;
    }

    char name[256];
    char type[64];
    SQLINTEGER prec, scale, maxlen;
    SQLLEN ind[5];
    SQLBindCol(stmt, 1, SQL_C_CHAR, name, sizeof(name), &ind[0]);
    SQLBindCol(stmt, 2, SQL_C_CHAR, type, sizeof(type), &ind[1]);
    SQLBindCol(stmt, 3, SQL_C_SLONG, &prec, 0, &ind[2]);
    SQLBindCol(stmt, 4, SQL_C_SLONG, &scale, 0, &ind[3]);
    SQLBindCol(stmt, 5, SQL_C_SLONG, &maxlen, 0, &ind[4]);

    sp_param_t* arr = NULL;
    size_t n = 0, cap = 0;
    while ((r = SQLFetch(stmt)) == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO) {
        if (ind[0] == SQL_NULL_DATA) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            sp_param_t* na = (sp_param_t*)realloc(arr, cap * sizeof(*arr));
            if (!na) {
                /* liberar lo acumulado */
                for (size_t k = 0; k < n; k++) {
                    free(arr[k].name);
                    free(arr[k].data_type);
                }
                free(arr);
                SQLFreeHandle(SQL_HANDLE_STMT, stmt);
                return CWS_ERR_NOMEM;
            }
            arr = na;
        }
        arr[n].name = strdup(name[0] == '@' ? name + 1 : name);
        arr[n].data_type = strdup(type);
        arr[n].precision = (int)prec;
        arr[n].scale = (int)scale;
        arr[n].max_length = (int)maxlen;
        if (!arr[n].name || !arr[n].data_type) {
            free(arr[n].name);
            free(arr[n].data_type);
            for (size_t k = 0; k < n; k++) {
                free(arr[k].name);
                free(arr[k].data_type);
            }
            free(arr);
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            return CWS_ERR_NOMEM;
        }
        n++;
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    *out = arr;
    *nout = n;
    return CWS_OK;
}

static void free_sp_params(sp_param_t* arr, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(arr[i].name);
        free(arr[i].data_type);
    }
    free(arr);
}

/* ------------------------------------------------------------------------- */
/* construcción de enlaces (plantilla + override del llamador)                */
/* ------------------------------------------------------------------------- */

static char* build_call(const char* sp_name, const sp_param_t* params,
                        size_t n) {
    /* EXEC <sp> + por parámetro ", @name=?" (el primero lleva espacio
     * inicial); se presupuestan 5 bytes por parámetro + margen. */
    size_t len = strlen("EXEC ") + strlen(sp_name) + 2;
    for (size_t i = 0; i < n; i++) len += strlen(params[i].name) + 6;
    char* s = (char*)malloc(len + 1);
    if (!s) return NULL;
    size_t off = 0;
    off += (size_t)sprintf(s + off, "EXEC %s", sp_name);
    for (size_t i = 0; i < n; i++) {
        if (i == 0)
            off += (size_t)sprintf(s + off, " @%s=?", params[i].name);
        else
            off += (size_t)sprintf(s + off, ", @%s=?", params[i].name);
    }
    return s;
}

static void apply_caller_param(bind_param_t* b, const sql_param_t* p) {
    switch (p->type) {
        case SQL_PT_NULL:
            b->is_null = 1;
            break;
        case SQL_PT_INT:
            b->is_null = 0;
            b->value_kind = VC_INT;
            b->i = p->val.as_int;
            break;
        case SQL_PT_DECIMAL:
            b->is_null = 0;
            b->value_kind = VC_DECIMAL;
            b->d = p->val.as_decimal;
            break;
        case SQL_PT_DATETIME:
            b->is_null = 0;
            b->value_kind = VC_DATETIME;
            datetime_to_ts(&p->val.as_datetime, &b->ts);
            break;
        case SQL_PT_STRING: {
            const char* s = p->val.as_string ? p->val.as_string : "";
            b->is_null = 0;
            if (b->is_unicode) {
                size_t cap = utf8_to_utf16(s, NULL, 0) + 1;
                uint16_t* ws = (uint16_t*)malloc(cap * sizeof(uint16_t));
                if (ws) {
                    size_t wlen = utf8_to_utf16(s, ws, cap);
                    ws[wlen] = 0;
                    free(b->ws);
                    b->ws = ws;
                    b->ws_bytes = (SQLLEN)(wlen * 2);
                    b->value_kind = VC_STRING_UTF16;
                }
            } else {
                free(b->str);
                b->str = strdup(s);
                b->value_kind = VC_STRING_ANSI;
            }
            break;
        }
        default:
            break;
    }
}

static void finalize_binding(bind_param_t* b) {
    if (b->is_null) {
        b->c_type = SQL_C_CHAR;
        b->ptr = NULL;
        b->ind = SQL_NULL_DATA;
        b->buf_len = 0;
        return;
    }
    switch (b->value_kind) {
        case VC_INT:
            b->c_type = SQL_C_SBIGINT;
            b->ptr = &b->i;
            b->ind = 0;
            b->buf_len = 0;
            break;
        case VC_DECIMAL:
            b->c_type = SQL_C_DOUBLE;
            b->ptr = &b->d;
            b->ind = 0;
            b->buf_len = 0;
            break;
        case VC_DATETIME:
            b->c_type = SQL_C_TIMESTAMP;
            b->ptr = &b->ts;
            b->ind = 0;
            b->buf_len = 0;
            break;
        case VC_STRING_ANSI:
            b->c_type = SQL_C_CHAR;
            b->ptr = b->str;
            b->ind = SQL_NTS;
            b->buf_len = b->str ? (SQLLEN)strlen(b->str) + 1 : 1;
            break;
        case VC_STRING_UTF16:
            b->c_type = SQL_C_WCHAR;
            b->ptr = b->ws;
            b->ind = b->ws_bytes;
            b->buf_len = b->ws_bytes + 2;
            break;
        default:
            b->c_type = SQL_C_CHAR;
            b->ptr = NULL;
            b->ind = SQL_NULL_DATA;
            b->buf_len = 0;
            break;
    }

    switch (b->sql_type) {
        case SQL_CHAR:
        case SQL_VARCHAR:
        case SQL_LONGVARCHAR:
            if (b->max_length > 0)
                b->column_size = (SQLULEN)b->max_length;
            else if (b->column_size == 0)
                b->column_size = 8000;
            if (b->value_kind == VC_STRING_ANSI && b->str &&
                (SQLULEN)strlen(b->str) > b->column_size)
                b->column_size = (SQLULEN)strlen(b->str);
            break;
        case SQL_WCHAR:
        case SQL_WVARCHAR:
        case SQL_WLONGVARCHAR:
            if (b->max_length > 0)
                b->column_size = (SQLULEN)(b->max_length / 2); /* bytes -> caracteres */
            else if (b->column_size == 0)
                b->column_size = 4000;
            else
                b->column_size /= 2;
            if (b->value_kind == VC_STRING_UTF16 && b->ws) {
                SQLLEN chars = b->ws_bytes / 2;
                if ((SQLULEN)chars > b->column_size) b->column_size = (SQLULEN)chars;
            }
            break;
        case SQL_INTEGER:
        case SQL_BIGINT:
        case SQL_SMALLINT:
        case SQL_TINYINT:
            if (b->column_size == 0) b->column_size = 10;
            break;
        case SQL_BIT:
            b->column_size = 1;
            break;
        case SQL_NUMERIC:
        case SQL_DECIMAL:
            if (b->column_size == 0) b->column_size = 18;
            break;
        case SQL_FLOAT:
        case SQL_REAL:
            b->column_size = 53;
            break;
        case SQL_TYPE_DATE:
            b->column_size = 10;
            b->decimal_digits = 0;
            break;
        case SQL_TYPE_TIME:
            b->column_size = 8;
            b->decimal_digits = 7;
            break;
        case SQL_TYPE_TIMESTAMP:
            b->column_size = 23;
            if (b->decimal_digits == 0) b->decimal_digits = 3;
            break;
        default:
            if (b->column_size == 0) b->column_size = 1;
            break;
    }
}

static void free_binds(bind_param_t* binds, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(binds[i].name);
        free(binds[i].str);
        free(binds[i].ws);
    }
    free(binds);
}

/* ------------------------------------------------------------------------- */
/* captura de un result set                                                   */
/* ------------------------------------------------------------------------- */

static int fetch_result_set(SQLHSTMT stmt, sql_result_t* res) {
    SQLRETURN r;
    SQLSMALLINT ncols = 0;

    memset(res, 0, sizeof(*res));
    r = SQLNumResultCols(stmt, &ncols);
    if (!SQL_SUCCEEDED(r) || ncols <= 0) return CWS_OK;

    out_col_t* cols = (out_col_t*)calloc((size_t)ncols, sizeof(out_col_t));
    sql_column_t* columns =
        (sql_column_t*)calloc((size_t)ncols, sizeof(sql_column_t));
    if (!cols || !columns) {
        free(cols);
        free(columns);
        return CWS_ERR_NOMEM;
    }
    res->ncols = (size_t)ncols;
    res->columns = columns;

    for (SQLSMALLINT i = 0; i < ncols; i++) {
        char name[SQL_EJECT_MAX_COL_NAME];
        SQLSMALLINT namelen, fsql, sc, nul;
        SQLULEN cs;
        out_col_t* oc = &cols[i];
        SQLDescribeCol(stmt, i + 1, (SQLCHAR*)name, sizeof(name), &namelen,
                       &fsql, &cs, &sc, &nul);
        name[SQL_EJECT_MAX_COL_NAME - 1] = '\0';
        if (!name[0]) snprintf(name, sizeof(name), "column_%d", i + 1);

        oc->name = strdup(name);
        oc->size = (size_t)cs;
        oc->scale = (int)sc;
        oc->type = out_type_of(fsql, &oc->is_unicode, &oc->is_binary);

        switch (oc->type) {
            case SQL_COL_INT:
                oc->c_type = SQL_C_SBIGINT;
                oc->buf_alloc = sizeof(int64_t);
                break;
            case SQL_COL_DECIMAL:
                oc->c_type = SQL_C_DOUBLE;
                oc->buf_alloc = sizeof(double);
                break;
            case SQL_COL_DATETIME:
                oc->c_type = SQL_C_TIMESTAMP;
                oc->buf_alloc = sizeof(SQL_TIMESTAMP_STRUCT);
                break;
            case SQL_COL_STRING:
            default:
                if (oc->is_binary) {
                    oc->c_type = SQL_C_BINARY;
                    oc->buf_alloc =
                        (cs > 0 && cs <= 1024 * 1024) ? (SQLLEN)cs : 4096;
                } else if (oc->is_unicode) {
                    oc->c_type = SQL_C_WCHAR;
                    oc->buf_alloc = (cs > 0 && cs <= 512 * 1024)
                                        ? (SQLLEN)(cs * 2 + 2)
                                        : 4096;
                } else {
                    oc->c_type = SQL_C_CHAR;
                    oc->buf_alloc =
                        (cs > 0 && cs <= 1024 * 1024) ? (SQLLEN)(cs + 1) : 4096;
                }
                break;
        }
        oc->buf = calloc(1, (size_t)oc->buf_alloc);
        if (!oc->buf) {
            for (SQLSMALLINT k = 0; k <= i; k++) {
                free(cols[k].name);
                free(cols[k].buf);
            }
            free(cols);
            free(columns);
            res->ncols = 0;
            res->columns = NULL;
            return CWS_ERR_NOMEM;
        }
        columns[i].name = oc->name;
        columns[i].type = oc->type;
        columns[i].size = oc->size;
        columns[i].scale = oc->scale;

        r = SQLBindCol(stmt, i + 1, oc->c_type, oc->buf, oc->buf_alloc,
                       &oc->ind);
        if (!SQL_SUCCEEDED(r)) {
            log_odbc_error(SQL_HANDLE_STMT, stmt, "SQLBindCol");
            for (SQLSMALLINT k = 0; k <= i; k++) {
                free(cols[k].name);
                free(cols[k].buf);
            }
            free(cols);
            free(columns);
            res->ncols = 0;
            res->columns = NULL;
            return CWS_ERR_GENERIC;
        }
    }

    size_t cap = SQL_EJECT_ROW_BATCH;
    size_t nrows = 0;
    sql_row_t* rows = (sql_row_t*)malloc(cap * sizeof(sql_row_t));
    if (!rows) {
        for (SQLSMALLINT k = 0; k < ncols; k++) {
            free(cols[k].name);
            free(cols[k].buf);
        }
        free(cols);
        free(columns);
        res->ncols = 0;
        res->columns = NULL;
        return CWS_ERR_NOMEM;
    }

    while ((r = SQLFetch(stmt)) == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO) {
        if (nrows == cap) {
            cap *= 2;
            sql_row_t* nr = (sql_row_t*)realloc(rows, cap * sizeof(sql_row_t));
            if (!nr) break;
            rows = nr;
        }
        sql_row_t row;
        row.ncols = (size_t)ncols;
        row.cols = (sql_value_t*)calloc((size_t)ncols, sizeof(sql_value_t));
        if (!row.cols) break;

        for (SQLSMALLINT i = 0; i < ncols; i++) {
            out_col_t* oc = &cols[i];
            sql_value_t* v = &row.cols[i];
            v->type = oc->type;
            if (oc->ind == SQL_NULL_DATA) {
                v->is_null = 1;
                continue;
            }

            if (oc->type == SQL_COL_STRING && oc->ind > oc->buf_alloc) {
                /* truncación: leer el resto con SQLGetData */
                SQLLEN total = oc->ind;
                void* nb = realloc(oc->buf, (size_t)total + 2);
                if (nb) {
                    oc->buf = nb;
                    size_t off = (size_t)oc->buf_alloc;
                    SQLLEN got = 0;
                    while (off < (size_t)total) {
                        r = SQLGetData(stmt, i + 1, oc->c_type,
                                       (char*)oc->buf + off,
                                       (SQLLEN)(total - off) + 2, &got);
                        if (r == SQL_SUCCESS || r == SQL_SUCCESS_WITH_INFO) {
                            if (got <= 0) break;
                            off += (size_t)got;
                        } else {
                            break;
                        }
                    }
                    ((char*)oc->buf)[total] = 0;
                    ((char*)oc->buf)[total + 1] = 0;
                    oc->buf_alloc = total;
                    oc->ind = total;
                }
            }

            switch (oc->type) {
                case SQL_COL_INT:
                    v->val.as_int = *(int64_t*)oc->buf;
                    break;
                case SQL_COL_DECIMAL:
                    v->val.as_decimal = *(double*)oc->buf;
                    break;
                case SQL_COL_DATETIME:
                    ts_to_datetime((SQL_TIMESTAMP_STRUCT*)oc->buf,
                                   &v->val.as_datetime);
                    break;
                case SQL_COL_STRING:
                default:
                    if (oc->is_binary) {
                        v->val.as_string =
                            hex_encode((const unsigned char*)oc->buf,
                                       (size_t)oc->ind);
                    } else if (oc->is_unicode) {
                        v->val.as_string =
                            utf16_to_utf8((const uint16_t*)oc->buf,
                                          (size_t)oc->ind / 2);
                    } else {
                        v->val.as_string = strdup((char*)oc->buf);
                    }
                    if (!v->val.as_string) {
                        v->is_null = 1;
                        v->type = SQL_COL_NULL;
                    }
                    break;
            }
        }
        rows[nrows++] = row;
    }

    res->rows = rows;
    res->nrows = nrows;

    for (SQLSMALLINT k = 0; k < ncols; k++)
        free(cols[k].buf);
    free(cols);
    return CWS_OK;
}

/* ------------------------------------------------------------------------- */
/* ejecución                                                                 */
/* ------------------------------------------------------------------------- */

static int run_store(SQLHDBC dbc, bind_param_t* binds, size_t nbinds,
                     const char* call, sql_result_t* out) {
    SQLRETURN r;
    SQLHSTMT stmt = SQL_NULL_HANDLE;

    r = SQLAllocHandle(SQL_HANDLE_STMT, dbc, &stmt);
    if (!SQL_SUCCEEDED(r)) {
        log_odbc_error(SQL_HANDLE_DBC, dbc, "SQLAllocHandle(STMT)");
        return CWS_ERR_GENERIC;
    }

    r = SQLPrepare(stmt, (SQLCHAR*)call, SQL_NTS);
    if (!SQL_SUCCEEDED(r)) {
        log_odbc_error(SQL_HANDLE_STMT, stmt, "SQLPrepare");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return CWS_ERR_GENERIC;
    }

    for (size_t i = 0; i < nbinds; i++) {
        bind_param_t* b = &binds[i];
        r = SQLBindParameter(stmt, (SQLUSMALLINT)(i + 1), SQL_PARAM_INPUT,
                             b->c_type, b->sql_type, b->column_size,
                             b->decimal_digits, b->ptr, b->buf_len, &b->ind);
        if (!SQL_SUCCEEDED(r)) {
            log_odbc_error(SQL_HANDLE_STMT, stmt, "SQLBindParameter");
            SQLFreeHandle(SQL_HANDLE_STMT, stmt);
            return CWS_ERR_GENERIC;
        }
    }

    r = SQLExecute(stmt);
    if (!SQL_SUCCEEDED(r)) {
        log_odbc_error(SQL_HANDLE_STMT, stmt, "SQLExecute");
        SQLFreeHandle(SQL_HANDLE_STMT, stmt);
        return CWS_ERR_GENERIC;
    }

    int rc = fetch_result_set(stmt, out);

    if (rc == CWS_OK) {
        while ((r = SQLMoreResults(stmt)) == SQL_SUCCESS) {
            SQLSMALLINT nc = 0;
            SQLNumResultCols(stmt, &nc);
            if (nc > 0) {
                SQLRETURN fr;
                while ((fr = SQLFetch(stmt)) == SQL_SUCCESS ||
                       fr == SQL_SUCCESS_WITH_INFO) {
                }
            }
        }
    }

    SQLFreeHandle(SQL_HANDLE_STMT, stmt);
    return rc;
}

/* ------------------------------------------------------------------------- */
/* API pública                                                               */
/* ------------------------------------------------------------------------- */

sql_eject_t* sql_eject_new(const app_config_t* cfg) {
    if (!cfg) return NULL;
    sql_eject_t* se = (sql_eject_t*)calloc(1, sizeof(*se));
    if (!se) return NULL;
    se->driver = strdup(cfg->db_driver);
    se->server = strdup(cfg->db_server);
    se->user = strdup(cfg->db_user);
    se->password = strdup(cfg->db_password);
    se->database = strdup(cfg->db_database);
    se->port = strdup(cfg->db_port);
    se->encrypt = cfg->db_encrypt;
    se->trust_cert = cfg->db_trust_cert;
    se->login_id = 1;
    if (!se->driver || !se->server || !se->user || !se->password ||
        !se->database || !se->port) {
        sql_eject_free(se);
        return NULL;
    }
    return se;
}

void sql_eject_free(sql_eject_t* se) {
    if (!se) return;
    free(se->driver);
    free(se->server);
    free(se->user);
    free(se->password);
    free(se->database);
    free(se->port);
    free(se);
}

void sql_eject_set_login_id(sql_eject_t* se, int64_t login_id) {
    if (se) se->login_id = login_id;
}

int sql_eject_store(sql_eject_t* se, const char* sp_name,
                    const char* database, const sql_param_t* params,
                    size_t nparams, sql_result_t* out) {
    if (!se || !sp_name || !database || !out) return CWS_ERR_INVALID;
    memset(out, 0, sizeof(*out));

    SQLHENV env = SQL_NULL_HANDLE;
    SQLHDBC dbc = SQL_NULL_HANDLE;
    int rc = se_connect(se, database, &env, &dbc);
    if (rc != CWS_OK) return rc;

    sp_param_t* schema = NULL;
    size_t nschema = 0;
    bind_param_t* binds = NULL;
    rc = fetch_sp_params(dbc, sp_name, &schema, &nschema);
    if (rc != CWS_OK) goto done;

    binds = (bind_param_t*)calloc(nschema ? nschema : 1, sizeof(bind_param_t));
    if (!binds) {
        rc = CWS_ERR_NOMEM;
        goto done;
    }

    for (size_t i = 0; i < nschema; i++) {
        const sp_param_t* sp = &schema[i];
        bind_param_t* b = &binds[i];
        int is_unicode = 0, is_int = 0, is_dt = 0;
        b->sql_type = odbc_type_for(sp->data_type, &is_unicode, &is_int, &is_dt);
        b->name = strdup(sp->name);
        if (!b->name) {
            rc = CWS_ERR_NOMEM;
            goto done;
        }
        b->is_unicode = is_unicode;
        b->column_size = (SQLULEN)sp->precision;
        b->decimal_digits = (SQLSMALLINT)sp->scale;
        b->max_length = sp->max_length;
        b->value_kind = value_class_of(b->sql_type, is_unicode);
        b->is_null = 1;

        /* plantilla por defecto */
        if (!strcmp(b->name, "usuario_alta") || !strcmp(b->name, "usuario_mod")) {
            b->is_null = 0;
            b->value_kind = VC_INT;
            b->i = se->login_id;
        } else if (b->value_kind == VC_DATETIME) {
            if (!strcmp(b->name, "fecha_alta") || !strcmp(b->name, "fecha_mod"))
                datetime_now(&b->ts);
            else {
                b->ts.year = 1900;
                b->ts.month = 1;
                b->ts.day = 1;
                b->ts.hour = 0;
                b->ts.minute = 0;
                b->ts.second = 0;
                b->ts.fraction = 0;
            }
            b->is_null = 0;
        } else if (b->value_kind == VC_INT) {
            b->i = 0;
            b->is_null = 0;
        } else if (b->value_kind == VC_DECIMAL) {
            b->d = 0.0;
            b->is_null = 0;
        }

        /* override del llamador */
        for (size_t k = 0; k < nparams; k++) {
            if (params[k].name &&
                !strcasecmp(b->name, params[k].name)) {
                apply_caller_param(b, &params[k]);
                break;
            }
        }
        finalize_binding(b);
    }

    char* call = build_call(sp_name, schema, nschema);
    if (!call) {
        rc = CWS_ERR_NOMEM;
        goto done;
    }

    rc = run_store(dbc, binds, nschema, call, out);

    free(call);

done:
    free_binds(binds, nschema);
    free_sp_params(schema, nschema);
    if (dbc != SQL_NULL_HANDLE) {
        SQLDisconnect(dbc);
        SQLFreeHandle(SQL_HANDLE_DBC, dbc);
    }
    if (env != SQL_NULL_HANDLE) SQLFreeHandle(SQL_HANDLE_ENV, env);
    return rc;
}

void sql_result_free(sql_result_t* res) {
    if (!res) return;
    for (size_t c = 0; c < res->ncols; c++)
        free((void*)res->columns[c].name);
    free(res->columns);
    for (size_t r = 0; r < res->nrows; r++) {
        for (size_t c = 0; c < res->rows[r].ncols; c++) {
            sql_value_t* v = &res->rows[r].cols[c];
            if (v->type == SQL_COL_STRING && !v->is_null)
                free(v->val.as_string);
        }
        free(res->rows[r].cols);
    }
    free(res->rows);
    memset(res, 0, sizeof(*res));
}

/* ------------------------------------------------------------------------- */
/* serialización JSON del result set                                          */
/* ------------------------------------------------------------------------- */

typedef struct json_buf {
    char*  data;
    size_t len;
    size_t cap;
} json_buf_t;

static int json_reserve(json_buf_t* jb, size_t extra) {
    if (jb->len + extra + 1 <= jb->cap) return 0;
    size_t ncap = jb->cap ? jb->cap : 256;
    while (ncap < jb->len + extra + 1) ncap *= 2;
    char* nd = (char*)realloc(jb->data, ncap);
    if (!nd) return -1;
    jb->data = nd;
    jb->cap = ncap;
    return 0;
}

static int json_append(json_buf_t* jb, const char* s, size_t n) {
    if (json_reserve(jb, n)) return -1;
    memcpy(jb->data + jb->len, s, n);
    jb->len += n;
    jb->data[jb->len] = '\0';
    return 0;
}

static int json_append_str(json_buf_t* jb, const char* s) {
    return json_append(jb, s, strlen(s));
}

static int json_escape(json_buf_t* jb, const char* s) {
    if (json_append(jb, "\"", 1)) return -1;
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        char esc[8];
        int n;
        switch (*p) {
            case '"':  n = snprintf(esc, sizeof(esc), "\\\""); break;
            case '\\': n = snprintf(esc, sizeof(esc), "\\\\"); break;
            case '\b': n = snprintf(esc, sizeof(esc), "\\b"); break;
            case '\f': n = snprintf(esc, sizeof(esc), "\\f"); break;
            case '\n': n = snprintf(esc, sizeof(esc), "\\n"); break;
            case '\r': n = snprintf(esc, sizeof(esc), "\\r"); break;
            case '\t': n = snprintf(esc, sizeof(esc), "\\t"); break;
            default:
                if (*p < 0x20) {
                    n = snprintf(esc, sizeof(esc), "\\u%04x", *p);
                } else {
                    if (json_append(jb, (const char*)p, 1)) return -1;
                    continue;
                }
                break;
        }
        if (json_append(jb, esc, (size_t)n)) return -1;
    }
    return json_append(jb, "\"", 1);
}

char* sql_result_to_json(const sql_result_t* res) {
    if (!res) return NULL;
    json_buf_t jb = {0};

    if (json_append(&jb, "[", 1)) goto oom;
    for (size_t r = 0; r < res->nrows; r++) {
        if (r) {
            if (json_append(&jb, ",", 1)) goto oom;
        }
        if (json_append(&jb, "{", 1)) goto oom;
        for (size_t c = 0; c < res->ncols; c++) {
            const char* key =
                res->columns[c].name ? res->columns[c].name : "";
            if (c) {
                if (json_append(&jb, ",", 1)) goto oom;
            }
            if (json_escape(&jb, key)) goto oom;
            if (json_append(&jb, ":", 1)) goto oom;

            const sql_value_t* v = &res->rows[r].cols[c];
            char num[64];
            if (v->is_null) {
                if (json_append(&jb, "null", 4)) goto oom;
            } else {
                switch (v->type) {
                    case SQL_COL_INT:
                        snprintf(num, sizeof(num), "%lld",
                                 (long long)v->val.as_int);
                        if (json_append(&jb, num, strlen(num))) goto oom;
                        break;
                    case SQL_COL_DECIMAL:
                        snprintf(num, sizeof(num), "%g", v->val.as_decimal);
                        if (json_append(&jb, num, strlen(num))) goto oom;
                        break;
                    case SQL_COL_STRING:
                        if (json_escape(&jb, v->val.as_string
                                              ? v->val.as_string
                                              : ""))
                            goto oom;
                        break;
                    case SQL_COL_DATETIME:
                        snprintf(num, sizeof(num), "\"%04d-%02d-%02dT%02d:%02d:%02d\"",
                                 v->val.as_datetime.year, v->val.as_datetime.month,
                                 v->val.as_datetime.day, v->val.as_datetime.hour,
                                 v->val.as_datetime.minute, v->val.as_datetime.second);
                        if (json_append(&jb, num, strlen(num))) goto oom;
                        break;
                    default:
                        if (json_append(&jb, "null", 4)) goto oom;
                        break;
                }
            }
        }
        if (json_append(&jb, "}", 1)) goto oom;
    }
    if (json_append(&jb, "]", 1)) goto oom;
    return jb.data;

oom:
    free(jb.data);
    return NULL;
}
