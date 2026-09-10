/*
 * configuration.c — configuración central de la aplicación.
 *
 * Espejo en C de streaming_server/src/server/app/core/configuration.js.
 * Las credenciales se leen del env cargado por cws (cws_app_env) o de
 * getenv() si `app` es NULL.
 *
 * El acceso a las variables pasa por cfg_getenv(), que despacha cada clave
 * con un switch sobre su hash FNV-1a 32 (core/hash_table.c). Los case son
 * constantes generadas con scripts/gen_env_hashes.sh (que corre LA MISMA
 * función hash); para agregar una clave nueva:
 *   1. ./scripts/gen_env_hashes.sh NUEVA_CLAVE
 *   2. pegar el case generado dentro del switch.
 */

#define _POSIX_C_SOURCE 200809L

#include "configuration.h"

#include <stdlib.h>
#include <string.h>

#include "hash_table.h"

/** App cuya env sirve cfg_getenv; la fija configuration_new(). */
static const cws_app_t* g_cfg_app = NULL;

static const char* env_get(const cws_app_t* app, const char* key) {
    if (app) return cws_app_env_get(app, key);
    return getenv(key);
}

static char* dup_or(const char* v, const char* def) {
    return strdup(v ? v : def);
}

/**
 * \brief Devuelve el valor de una variable del .env por su clave.
 *
 * Despacha con un switch sobre hash_string(clave); los case son los
 * valores generados por scripts/gen_env_hashes.sh. Claves desconocidas
 * caen al default (comparación directa). Si configuration_new() aún no
 * corrió, lee del entorno del proceso (getenv).
 */
const char* cfg_getenv(const char* key) {
    const cws_app_t* app = g_cfg_app;
    if (!key) return NULL;
    switch (hash_string(key)) {
        /* Tabla generada por scripts/gen_env_hashes.sh — no editar a mano
         * los valores; regenerar con el script si cambia una clave. */
        case 0xde658145u: /* ACCESS_TOKEN_EXPIRATION_MINUTES */
            return env_get(app, "ACCESS_TOKEN_EXPIRATION_MINUTES");
        case 0x54dd7f29u: /* DB_DATABASE */
            return env_get(app, "DB_DATABASE");
        case 0xbf43c7aau: /* DB_DRIVER */
            return env_get(app, "DB_DRIVER");
        case 0xef881b7du: /* DB_ENCRYPT */
            return env_get(app, "DB_ENCRYPT");
        case 0x1d6944b5u: /* DB_PASSWORD */
            return env_get(app, "DB_PASSWORD");
        case 0x2f5665bfu: /* DB_PORT */
            return env_get(app, "DB_PORT");
        case 0x06be4937u: /* DB_SERVER */
            return env_get(app, "DB_SERVER");
        case 0x358104f2u: /* DB_TRUST_CERTIFICATE */
            return env_get(app, "DB_TRUST_CERTIFICATE");
        case 0x081a24f7u: /* DB_USER */
            return env_get(app, "DB_USER");
        case 0x4de9002au: /* HLS_DIR */
            return env_get(app, "HLS_DIR");
        case 0x24fff026u: /* PORT */
            return env_get(app, "PORT");
        case 0x7d2d4c55u: /* PUBLIC_ID_LENGTH */
            return env_get(app, "PUBLIC_ID_LENGTH");
        case 0xb61046b0u: /* REFRESH_TOKEN_EXPIRATION_DAYS */
            return env_get(app, "REFRESH_TOKEN_EXPIRATION_DAYS");
        case 0x90bd39a1u: /* SECRET_KEY */
            return env_get(app, "SECRET_KEY");
        case 0xd4d08bc7u: /* X_VECTOR */
            return env_get(app, "X_VECTOR");
        case 0xd5e15ec9u: /* NODE_ENV */
            return env_get(app, "NODE_ENV");
        case 0xc5a91690u: /* SERVER_PORT */
            return env_get(app, "SERVER_PORT");
        default:
            return env_get(app, key);
    }
}

app_config_t* configuration_new(const cws_app_t* app) {
    app_config_t* cfg = (app_config_t*)calloc(1, sizeof(*cfg));
    if (!cfg) return NULL;
    /* cfg_getenv() queda ligado a esta env a partir de ahora. */
    if (app) g_cfg_app = app;

    const char* port = cfg_getenv("SERVER_PORT");
    if (!port || !*port) port = cfg_getenv("PORT");
    if (!port || !*port) port = "8181";
    cfg->port = atoi(port);
    if (cfg->port < 1 || cfg->port > 65535) cfg->port = 8181;

    cfg->db_driver = dup_or(cfg_getenv("DB_DRIVER"), "ODBC Driver 17 for SQL Server");
    cfg->db_server = dup_or(cfg_getenv("DB_SERVER"), "127.0.0.1");
    cfg->db_port = dup_or(cfg_getenv("DB_PORT"), "1433");
    cfg->db_user = dup_or(cfg_getenv("DB_USER"), "");
    cfg->db_password = dup_or(cfg_getenv("DB_PASSWORD"), "");
    cfg->db_database = dup_or(cfg_getenv("DB_DATABASE"), "");

    cfg->secret_key = dup_or(cfg_getenv("SECRET_KEY"), "");
    cfg->x_vector = dup_or(cfg_getenv("X_VECTOR"), "");
    const char* atm = cfg_getenv("ACCESS_TOKEN_EXPIRATION_MINUTES");
    cfg->access_token_expiration_minutes = atm ? atoi(atm) : 15;
    if (cfg->access_token_expiration_minutes < 1)
        cfg->access_token_expiration_minutes = 15;
    const char* rtd = cfg_getenv("REFRESH_TOKEN_EXPIRATION_DAYS");
    cfg->refresh_token_expiration_days = rtd ? atoi(rtd) : 7;
    if (cfg->refresh_token_expiration_days < 1)
        cfg->refresh_token_expiration_days = 7;
    const char* env_name = cfg_getenv("NODE_ENV");
    cfg->is_production = (env_name && strcmp(env_name, "production") == 0) ||
                         !env_name;

    const char* e = cfg_getenv("DB_ENCRYPT");
    cfg->db_encrypt = !(e && !strcmp(e, "false"));
    const char* t = cfg_getenv("DB_TRUST_CERTIFICATE");
    cfg->db_trust_cert = !(t && !strcmp(t, "false"));

    if (!cfg->db_driver || !cfg->db_server || !cfg->db_port ||
        !cfg->db_user || !cfg->db_password || !cfg->db_database ||
        !cfg->secret_key || !cfg->x_vector) {
        configuration_free(cfg);
        return NULL;
    }
    return cfg;
}

void configuration_free(app_config_t* cfg) {
    if (!cfg) return;
    free(cfg->db_driver);
    free(cfg->db_server);
    free(cfg->db_port);
    free(cfg->db_user);
    free(cfg->db_password);
    free(cfg->db_database);
    free(cfg->secret_key);
    free(cfg->x_vector);
    free(cfg);
}
