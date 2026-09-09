/*
 * configuration.c — configuración central de la aplicación.
 *
 * Espejo en C de streaming_server/src/server/app/core/configuration.js.
 * Las credenciales se leen del env cargado por cws (cws_app_env) o de
 * getenv() si `app` es NULL.
 */

#define _POSIX_C_SOURCE 200809L

#include "configuration.h"

#include <stdlib.h>
#include <string.h>

static const char* env_get(const cws_app_t* app, const char* key) {
    if (app) return cws_app_env_get(app, key);
    return getenv(key);
}

static const char* env_get_or(const cws_app_t* app, const char* key,
                              const char* def) {
    const char* v = env_get(app, key);
    return (v && *v) ? v : def;
}

static char* dup_or(const char* v, const char* def) {
    return strdup(v ? v : def);
}

app_config_t* configuration_new(const cws_app_t* app) {
    app_config_t* cfg = (app_config_t*)calloc(1, sizeof(*cfg));
    if (!cfg) return NULL;

    const char* port = env_get_or(app, "SERVER_PORT", NULL);
    if (!port || !*port) port = env_get_or(app, "PORT", "8181");
    cfg->port = atoi(port);
    if (cfg->port < 1 || cfg->port > 65535) cfg->port = 8181;

    cfg->db_driver = dup_or(env_get_or(app, "DB_DRIVER", NULL),
                            "ODBC Driver 17 for SQL Server");
    cfg->db_server = dup_or(env_get_or(app, "DB_SERVER", NULL), "127.0.0.1");
    cfg->db_port = dup_or(env_get_or(app, "DB_PORT", NULL), "1433");
    cfg->db_user = dup_or(env_get_or(app, "DB_USER", NULL), "");
    cfg->db_password = dup_or(env_get_or(app, "DB_PASSWORD", NULL), "");
    cfg->db_database = dup_or(env_get_or(app, "DB_DATABASE", NULL), "");

    cfg->secret_key = dup_or(env_get_or(app, "SECRET_KEY", NULL), "");
    cfg->x_vector = dup_or(env_get_or(app, "X_VECTOR", NULL), "");
    const char* atm = env_get_or(app, "ACCESS_TOKEN_EXPIRATION_MINUTES", "15");
    cfg->access_token_expiration_minutes = atoi(atm);
    if (cfg->access_token_expiration_minutes < 1)
        cfg->access_token_expiration_minutes = 15;
    const char* rtd = env_get_or(app, "REFRESH_TOKEN_EXPIRATION_DAYS", NULL);
    cfg->refresh_token_expiration_days = rtd ? atoi(rtd) : 7;
    if (cfg->refresh_token_expiration_days < 1)
        cfg->refresh_token_expiration_days = 7;
    const char* env_name = env_get_or(app, "NODE_ENV", "production");
    cfg->is_production = strcmp(env_name, "production") == 0;

    const char* e = env_get(app, "DB_ENCRYPT");
    cfg->db_encrypt = !(e && !strcmp(e, "false"));
    const char* t = env_get(app, "DB_TRUST_CERTIFICATE");
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
