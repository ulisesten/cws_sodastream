#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Configuración central de la aplicación.
 *
 * Equivalente a streaming_server/src/server/app/core/configuration.js
 * (dotenv incluido: el módulo carga el .env y lo exporta al entorno del
 * proceso en su primer uso, como dotenv.config() muta process.env).
 *
 * No depende del handle del app de cws: el .env es propiedad del módulo.
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

/**
 * \brief Construye la configuración leyendo el .env y el entorno.
 *
 * En su primer uso carga ".env" (cwd) y lo exporta al entorno del proceso;
 * las llamadas siguientes reutilizan el env ya cargado. Si el archivo no
 * existe, se usa solo el entorno del proceso.
 *
 * \return config malloc (liberar con configuration_free), o NULL.
 */
app_config_t* configuration_new(void);
void          configuration_free(app_config_t* cfg);

/**
 * \brief Valor de una variable del .env/entorno por su clave (equivalente
 *        a process.env en la referencia).
 *
 * Despacha la clave con un switch sobre su hash (core/hash_table.c); los
 * case son constantes generadas por scripts/gen_env_hashes.sh. Claves
 * desconocidas caen a una comparación directa (fallback del default).
 *
 * \param[in] key clave, p. ej. "DB_USER" o "PORT".
 * \return valor o NULL si no está configurada. Válido hasta el fin del
 *         proceso (o configuration_shutdown).
 */
const char* cfg_getenv(const char* key);

/** Libera el env cargado por el módulo (opcional; al cierre). */
void configuration_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* CONFIGURATION_H */
