/*
 * users_domain.c — acceso a datos del módulo de users.
 *
 * Espejo en C de reference/users/domain/users.domain.js. Ejecuta los
 * stored procedures procUsersProc (registro) y procUsersCons (consulta)
 * vía core/sql_eject (ODBC).
 */

#define _POSIX_C_SOURCE 200809L

#include "users_domain.h"

#include <stdlib.h>
#include <string.h>

#include "configuration.h"

#define USERS_DB "soda_stream"

/** PROC_USU_REGISTRAR: tipoRegistro del alta de usuario. */
#define PROC_USU_REGISTRAR 1
/** CONS_USU_SIGNIN: tipoConsulta de signin. */
#define CONS_USU_SIGNIN 1

static sql_eject_t* g_sql = NULL;

void users_domain_init(void) {
    app_config_t* cfg = configuration_new();
    if (!cfg) return;
    sql_eject_t* se = sql_eject_new(cfg);
    configuration_free(cfg);
    if (!se) return;
    sql_eject_free(g_sql);
    g_sql = se;
}

void users_domain_shutdown(void) {
    sql_eject_free(g_sql);
    g_sql = NULL;
}

sql_result_t* users_domain_new(const char* usu_nombre,
                               const char* usu_ape_paterno,
                               const char* usu_ape_materno,
                               const char* hashed_correo,
                               const char* hashed_contrasena,
                               const char* usu_salt) {
    if (!g_sql || !hashed_correo || !hashed_contrasena || !usu_salt)
        return NULL;

    /* Opcionales (undefined en JS) -> SQL NULL. */
    sql_param_t params[7];
    size_t n = 0;

    params[n].name = "tipoRegistro";
    params[n].type = SQL_PT_INT;
    params[n].val.as_int = PROC_USU_REGISTRAR;
    n++;

    const char* strings[] = {
        usu_nombre, usu_ape_paterno, usu_ape_materno,
        hashed_correo, hashed_contrasena, usu_salt,
    };
    const char* names[] = {
        "usu_nombre", "usu_ape_paterno", "usu_ape_materno",
        "usu_correo", "usu_contrasena", "usu_salt",
    };
    for (int i = 0; i < 6; i++) {
        params[n].name = names[i];
        params[n].type = strings[i] ? SQL_PT_STRING : SQL_PT_NULL;
        params[n].val.as_string = strings[i];
        n++;
    }

    sql_result_t* out = (sql_result_t*)calloc(1, sizeof(*out));
    if (!out) return NULL;
    int rc = sql_eject_store(g_sql, "procUsersProc", USERS_DB, params, n,
                             out);
    if (rc != CWS_OK) {
        sql_result_free(out);
        return NULL;
    }
    return out;
}

sql_result_t* users_domain_signin(const char* hashed_correo) {
    if (!g_sql || !hashed_correo) return NULL;

    sql_param_t params[2];
    params[0].name = "tipoConsulta";
    params[0].type = SQL_PT_INT;
    params[0].val.as_int = CONS_USU_SIGNIN;
    params[1].name = "usu_correo";
    params[1].type = SQL_PT_STRING;
    params[1].val.as_string = hashed_correo;

    sql_result_t* out = (sql_result_t*)calloc(1, sizeof(*out));
    if (!out) return NULL;
    int rc = sql_eject_store(g_sql, "procUsersCons", USERS_DB, params, 2,
                             out);
    if (rc != CWS_OK) {
        sql_result_free(out);
        return NULL;
    }
    return out;
}
