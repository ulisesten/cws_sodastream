/*
 * configuration.c — configuración central de la aplicación.
 *
 * Espejo en C de streaming_server/src/server/app/core/configuration.js.
 * El módulo es dueño del .env: lo carga (cws_env) y lo exporta al entorno
 * del proceso en su primer uso — como dotenv.config() muta process.env —
 * así que no necesita el handle del app de cws en ningún lado.
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

#include <cws/env.h>

#include "hash_table.h"

/** Env del módulo: cargado una sola vez, propiedad de configuration. */
static cws_env_t* g_env = NULL;

static char* dup_or(const char* v, const char* def) {
    return strdup(v ? v : def);
}

/* Valor de `key`: primero el .env del módulo, luego el entorno del
 * proceso (que ya incluye lo exportado del .env). */
static const char* env_get(const char* key) {
    if (g_env) {
        const char* v = cws_env_get(g_env, key);
        if (v && *v) return v;
    }
    return getenv(key);
}

/* dotenv.config(): carga ".env" y exporta sus valores al entorno del
 * proceso. Idempotente. */
static void load_env_once(void) {
    if (g_env) return;
    g_env = cws_env_new();
    if (!g_env) return;
    cws_env_load_file(g_env, ".env");   /* leniente: ignora archivo ausente */
    cws_env_export_to_environ(g_env);
}

/**
 * \brief Devuelve el valor de una variable del .env por su clave.
 *
 * Despacha con un switch sobre hash_string(clave); los case son los
 * valores generados por scripts/gen_env_hashes.sh. Claves desconocidas
 * caen al default (comparación directa).
 */
const char* cfg_getenv(const char* key) {
    if (!key) return NULL;
    load_env_once();
    switch (hash_string(key)) {
        /* Tabla generada por scripts/gen_env_hashes.sh — no editar a mano
         * los valores; regenerar con el script si cambia una clave. */
        case 0xde658145u: /* ACCESS_TOKEN_EXPIRATION_MINUTES */
            return env_get("ACCESS_TOKEN_EXPIRATION_MINUTES");
        case 0x54dd7f29u: /* DB_DATABASE */
            return env_get("DB_DATABASE");
        case 0xbf43c7aau: /* DB_DRIVER */
            return env_get("DB_DRIVER");
        case 0xef881b7du: /* DB_ENCRYPT */
            return env_get("DB_ENCRYPT");
        case 0x1d6944b5u: /* DB_PASSWORD */
            return env_get("DB_PASSWORD");
        case 0x2f5665bfu: /* DB_PORT */
            return env_get("DB_PORT");
        case 0x06be4937u: /* DB_SERVER */
            return env_get("DB_SERVER");
        case 0x358104f2u: /* DB_TRUST_CERTIFICATE */
            return env_get("DB_TRUST_CERTIFICATE");
        case 0x081a24f7u: /* DB_USER */
            return env_get("DB_USER");
        case 0x4de9002au: /* HLS_DIR */
            return env_get("HLS_DIR");
        case 0x24fff026u: /* PORT */
            return env_get("PORT");
        case 0x7d2d4c55u: /* PUBLIC_ID_LENGTH */
            return env_get("PUBLIC_ID_LENGTH");
        case 0xb61046b0u: /* REFRESH_TOKEN_EXPIRATION_DAYS */
            return env_get("REFRESH_TOKEN_EXPIRATION_DAYS");
        case 0x90bd39a1u: /* SECRET_KEY */
            return env_get("SECRET_KEY");
        case 0xd4d08bc7u: /* X_VECTOR */
            return env_get("X_VECTOR");
        case 0xd5e15ec9u: /* NODE_ENV */
            return env_get("NODE_ENV");
        case 0xc5a91690u: /* SERVER_PORT */
            return env_get("SERVER_PORT");
        case 0xa33522bbu: /* WEB_SESSION_TYPE */
            return env_get("WEB_SESSION_TYPE");
        case 0xbca4f0e9u: /* MOBILE_SESSION_TYPE */
            return env_get("MOBILE_SESSION_TYPE");
        case 0x57c85321u: /* PASSWORD_KDF_ITERATIONS */
            return env_get("PASSWORD_KDF_ITERATIONS");
        case 0x8e3a3a39u: /* PASSWORD_PEPPER */
            return env_get("PASSWORD_PEPPER");
        case 0xec4e5010u: /* CORS_ORIGINS */
            return env_get("CORS_ORIGINS");
        case 0x096d198cu: /* COOKIE_DOMAIN */
            return env_get("COOKIE_DOMAIN");
        case 0x4d74448eu: /* API_NAS */
            return env_get("API_NAS");
        case 0x0923b8fdu: /* STICKERS_DIR */
            return env_get("STICKERS_DIR");
        case 0x3ae54f82u: /* TELEGRAM_BOT_TOKEN */
            return env_get("TELEGRAM_BOT_TOKEN");
        case 0x914cd6c9u: /* TELEGRAM_BOT_USERNAME */
            return env_get("TELEGRAM_BOT_USERNAME");
        case 0xee81b94fu: /* TELEGRAM_API_BASE */
            return env_get("TELEGRAM_API_BASE");
        case 0x701f8492u: /* TELEGRAM_POLL_TIMEOUT */
            return env_get("TELEGRAM_POLL_TIMEOUT");
        case 0x0237c83au: /* TELEGRAM_NOTIFY_ALWAYS */
            return env_get("TELEGRAM_NOTIFY_ALWAYS");
        case 0x35953fe3u: /* TELEGRAM_LINK_TTL_MIN */
            return env_get("TELEGRAM_LINK_TTL_MIN");
        case 0x9d082a86u: /* TELEGRAM_CALL_URI */
            return env_get("TELEGRAM_CALL_URI");
        case 0xa67ea4c6u: /* EXTERNAL_API_SECRET */
            return env_get("EXTERNAL_API_SECRET");
        case 0x80c97130u: /* STUN_URLS */
            return env_get("STUN_URLS");
        case 0x1dbda671u: /* TURN_URLS */
            return env_get("TURN_URLS");
        case 0x4ef30257u: /* TURN_SECRET */
            return env_get("TURN_SECRET");
        case 0x67a029c7u: /* TURN_TTL_SECONDS */
            return env_get("TURN_TTL_SECONDS");
        case 0xd1efc799u: /* CALL_E2EE_REQUIRED */
            return env_get("CALL_E2EE_REQUIRED");
        case 0x38cde61bu: /* CALL_ID_SERVER */
            return env_get("CALL_ID_SERVER");
        case 0xd558e0ffu: /* FCM_ENABLED */
            return env_get("FCM_ENABLED");
        case 0xf7af9049u: /* FCM_PROJECT_ID */
            return env_get("FCM_PROJECT_ID");
        case 0x0e6b2754u: /* FCM_CREDENTIALS */
            return env_get("FCM_CREDENTIALS");
        case 0xc177dd39u: /* FCM_DRY_RUN */
            return env_get("FCM_DRY_RUN");
        case 0xdf6168fau: /* FCM_MAX_DEVICES_PER_USER */
            return env_get("FCM_MAX_DEVICES_PER_USER");
        case 0x45f28955u: /* FCM_RATE_PER_MINUTE */
            return env_get("FCM_RATE_PER_MINUTE");
        default:
            return env_get(key);
    }
}

app_config_t* configuration_new(void) {
    load_env_once();

    app_config_t* cfg = (app_config_t*)calloc(1, sizeof(*cfg));
    if (!cfg) return NULL;

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
    cfg->is_production = !env_name || strcmp(env_name, "production") == 0;

    /* Session types — configurables vía .env (WEB_SESSION_TYPE /
     * MOBILE_SESSION_TYPE) con defaults 1/2 (compat con la referencia). */
    cfg->web_session_type = 1;
    const char* wst = cfg_getenv("WEB_SESSION_TYPE");
    if (wst && *wst) cfg->web_session_type = (uint32_t)strtoul(wst, NULL, 10);
    cfg->mobile_session_type = 2;
    const char* mst = cfg_getenv("MOBILE_SESSION_TYPE");
    if (mst && *mst) cfg->mobile_session_type = (uint32_t)strtoul(mst, NULL, 10);

    /* NAS de miniaturas (API_NAS en la referencia; vacío = sin NAS). */
    cfg->api_nas = dup_or(cfg_getenv("API_NAS"), "");

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
    free(cfg->api_nas);
    free(cfg);
}

void configuration_shutdown(void) {
    cws_env_free(g_env);
    g_env = NULL;
}
