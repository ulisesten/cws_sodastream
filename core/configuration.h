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
} app_config_t;

/* `app` puede ser NULL para leer de getenv() directamente. */
app_config_t* configuration_new(const cws_app_t* app);
void          configuration_free(app_config_t* cfg);

#ifdef __cplusplus
}
#endif

#endif /* CONFIGURATION_H */
