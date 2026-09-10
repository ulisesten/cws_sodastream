#ifndef AUTHORIZATION_H
#define AUTHORIZATION_H

#include <stdbool.h>

#include "cws/cws.h"
#include "configuration.h"
#include "jwt.h"
#include "sql_eject.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * AuthorizationService — autorización de peticiones y signin.
 *
 * Adaptación en C de reference/authorization.js (class AuthorizationService):
 *
 *   authorization.js                        authorization.c
 *   ─────────────────────────────────────   ─────────────────────────────────
 *   userDAO(correo)                      →  authorization_user_dao()
 *     (sqlEject.store_eject("procUsersCons",
 *      {tipoConsulta:1, usu_correo}))
 *   verify(req, res, next)               →  cws_mw_authorization_verify()
 *     (cookie access_token + header        (cookie + header x-csrf-token,
 *      x-csrf-token, salt del DAO)         salt del DAO)
 *   user_signin(req, res)                →  authorization_signin()
 *     (hash(email), verify de contraseña   (4 cookies Set-Cookie)
 *      y 4 cookies)
 *   refresh(req, res, next)              →  cws_mw_authorization_refresh()
 *
 * Estado de la petición: tras verify/refresh, el usuario autorizado queda
 * en req->__user y se consulta con authorization_request_user() /
 * authorization_request_authorized() (equivalente a req.user/req.authorized).
 */

/** CONS_USU_SIGNIN: tipoConsulta del DAO de usuario. */
#define AUTH_CONS_USU_SIGNIN 1

/** Base donde viven los stored procedures de usuarios. */
#define AUTH_DB "soda_stream"

typedef struct authorization authorization_t;

/* ------------------------------------------------------------------ */
/* ciclo de vida                                                       */
/* ------------------------------------------------------------------ */

/**
 * \brief Construye el servicio (crea su jwt_t* y su sql_eject_t*).
 * NULL si falta configuración o falla la construcción de los submódulos.
 */
authorization_t* authorization_new(const app_config_t* cfg);
void authorization_free(authorization_t* auth);

/**
 * \brief Instancia global para los middlewares (patrón videos_domain_init):
 * construye el servicio desde el env del app. Llamar una vez en main() tras
 * cargar el .env. authorization_shutdown() la libera.
 */
void authorization_init(void);
void authorization_shutdown(void);

/* ------------------------------------------------------------------ */
/* DAO                                                                 */
/* ------------------------------------------------------------------ */

/**
 * \brief userDAO: ejecuta procUsersCons(tipoConsulta=1, usu_correo).
 *
 * En la referencia, para signin el correo se busca POR SU HASH
 * (Streebog-256) y en verify se busca el correo en claro.
 *
 * \return resultado malloc (liberar con sql_result_free), NULL en fallo.
 */
sql_result_t* authorization_user_dao(authorization_t* auth,
                                     const char* usu_correo);

/* ------------------------------------------------------------------ */
/* middlewares (cws_app_use)                                           */
/* ------------------------------------------------------------------ */

/**
 * \brief verify(req,res,next): exige cookie access_token y header
 * x-csrf-token; valida ambos tokens, revalida el salt contra el DAO y
 * publica el usuario en req->__user. Responde 401 JSON si falla.
 */
void cws_mw_authorization_verify(cws_request_t* req, cws_response_t* res,
                                 cws_next_fn next);

/**
 * \brief refresh(req,res,next): exige cookie refresh_token y header
 * x-csrf-token (refresh_csrf); re-emite access_token/csrf_token/
 * refresh_csrf_token y publica el usuario en req->__user.
 */
void cws_mw_authorization_refresh(cws_request_t* req, cws_response_t* res,
                                  cws_next_fn next);

/* ------------------------------------------------------------------ */
/* signin                                                              */
/* ------------------------------------------------------------------ */

/**
 * \brief user_signin: valida credenciales del body JSON
 * ({"usu_correo":"...","usu_contrasena":"..."}) y emite las 4 cookies
 * (access_token, csrf_token, refresh_token, refresh_csrf_token).
 *
 * El correo se busca por su hash Streebog-256 (igual que la referencia) y
 * la contraseña se verifica con jwt_gost_hash_verify contra
 * dao.usu_contrasena.
 *
 * \param[out] out usuario autenticado (campos malloc; liberar con
 *                 authorization_user_free) solo si devuelve CWS_OK.
 * \return CWS_OK; CWS_ERR_NOTFOUND si las credenciales no pasan (el
 *         llamador responde 401); CWS_ERR_* en otro fallo.
 */
int authorization_signin(cws_request_t* req, cws_response_t* res,
                         jwt_user_t* out);

/** Libera los campos de un jwt_user_t producido por authorization_signin. */
void authorization_user_free(jwt_user_t* user);

/* ------------------------------------------------------------------ */
/* estado de la petición (req.user / req.authorized)                   */
/* ------------------------------------------------------------------ */

/** Usuario autorizado por el middleware (propiedad de req->__user). */
const jwt_user_t* authorization_request_user(const cws_request_t* req);

/** true si el middleware autorizó la petición. */
bool authorization_request_authorized(const cws_request_t* req);

#ifdef __cplusplus
}
#endif

#endif /* AUTHORIZATION_H */
