#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include "cws/cws.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Configuración central de la aplicación.
 *
 * Equivalente a streaming_server/src/server/app/core/configuration.js.
 * Lee las variables de entorno (o del .env cargado por el app cws) una sola
 * vez y las expone como struct. Las cadenas son propiedad del struct y deben
 * liberarse con configuration_free().
 */

typedef struct app_config {
    int   port;              /* SERVER_PORT / PORT, default 8181 */
    char* db_driver;         /* DB_DRIVER, default "ODBC Driver 17 for SQL Server" */
    char* db_server;         /* DB_SERVER, default "127.0.0.1" */
    char* db_port;           /* DB_PORT, default "1433" */
    char* db_user;           /* DB_USER */
    char* db_password;       /* DB_PASSWORD */
    char* db_database;       /* DB_DATABASE */
    int   db_encrypt;        /* DB_ENCRYPT != "false" */
    int   db_trust_cert;     /* DB_TRUST_CERTIFICATE != "false" */

    /* Tokens / sesión (jwt.c, authorization.c). */
    char* secret_key;        /* SECRET_KEY: clave del JWT y derivación del IV */
    char* x_vector;          /* X_VECTOR: clave GOST 28147-89 (8 palabras hex
                                  separadas por coma) */
    int   access_token_expiration_minutes; /* ACCESS_TOKEN_EXPIRATION_MINUTES, default 15 */
    int   refresh_token_expiration_days;   /* REFRESH_TOKEN_EXPIRATION_DAYS, default 7 */
    int   is_production;     /* NODE_ENV == "production" (cookies secure) */
} app_config_t;

/* `app` puede ser NULL para leer de getenv() directamente. */
app_config_t* configuration_new(const cws_app_t* app);
void          configuration_free(app_config_t* cfg);

/**
 * \brief Valor de una variable del .env por su clave (equivalente a
 *        process.env en la referencia).
 *
 * Despacha la clave con un switch sobre su hash (core/hash_table.c); los
 * case son constantes generadas por scripts/gen_env_hashes.sh. Claves
 * desconocidas caen a una comparación directa (fallback del default).
 *
 * \param[in] key clave, p. ej. "DB_USER" o "PORT".
 * \return valor o NULL si no está configurada. El puntero es propiedad
 *         del env (válido hasta que el app se libere).
 */
const char* cfg_getenv(const char* key);

#ifdef __cplusplus
}
#endif

#endif /* CONFIGURATION_H */
