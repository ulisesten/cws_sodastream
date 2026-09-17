#ifndef AUTHORIZATION_MOBILE_H
#define AUTHORIZATION_MOBILE_H

#include <stdbool.h>

#include "cws/cws.h"
#include "configuration.h"
#include "jwt_mobile.h"
#include "sql_eject.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * AuthorizationMobile — autenticación para apps Android (sin cookies).
 *
 * Adaptación del flujo de referencia (streaming_server) para móvil:
 *   - signin: valida credenciales y emite access + refresh tokens GOST
 *     (canal jwt_mobile). Los tokens se devuelven en el JSON body (no hay
 *     cookies); el cliente móvil los guarda y los envía en el header
 *     `Authorization: Bearer <token>`.
 *   - middleware verify: lee el Bearer header, NO valida IP ni CSRF
 *     (no aplica a móvil).
 *   - refresh: lee refresh token vía header (no cookie), emite nuevo
 *     access token.
 *
 * Las primitivas criptográficas y la firma son las mismas que jwt (web),
 * pero con JWT_MOBILE_SESSION_TYPE (cfg->mobile_session_type, default 2).
 */

#define AUTH_MOBILE_DB "soda_stream"

typedef struct authorization_mobile authorization_mobile_t;

/* ------------------------------------------------------------------ */
/* ciclo de vida                                                       */
/* ------------------------------------------------------------------ */

authorization_mobile_t* authorization_mobile_new(const app_config_t* cfg);
void authorization_mobile_free(authorization_mobile_t* auth);

/** Instancia global para el middleware (patrón videos_domain_init).
 *  \return 1 si quedó inicializado; 0 si falta configuración. */
int  authorization_mobile_init(void);
void authorization_mobile_shutdown(void);

/* ------------------------------------------------------------------ */
/* DAO (mismo procUsersCons que el web)                                */
/* ------------------------------------------------------------------ */

sql_result_t* authorization_mobile_user_dao(authorization_mobile_t* auth,
                                           const char* usu_correo);

/* ------------------------------------------------------------------ */
/* middleware                                                          */
/* ------------------------------------------------------------------ */

/**
 * \brief verify(req,res,next): exige header Authorization: Bearer
 *        <access_token> + revalidación de salt contra el DAO. No usa
 *        cookies, ni CSRF, ni valida IP.
 */
void cws_mw_authorization_mobile_verify(cws_request_t* req, cws_response_t* res,
                                        cws_next_fn next);

/**
 * \brief refresh(req,res): endpoint de refresh. Lee el refresh_token del
 *        header X-Refresh-Token, lo valida y responde en el body un nuevo
 *        access_token (sin cookies ni CSRF).
 */
void authorization_mobile_refresh(cws_request_t* req, cws_response_t* res);

/**
 * \brief logout(req,res): cierra sesión (Bearer). Invalida los tokens del
 *        usuario rotando su `usu_salt` y responde JSON.
 */
void authorization_mobile_logout(cws_request_t* req, cws_response_t* res);

/* ------------------------------------------------------------------ */
/* signin                                                              */
/* ------------------------------------------------------------------ */

/**
 * \brief user_signin: valida body {usu_correo, usu_contrasena} y emite
 *        access + refresh (canal mobile). Devuelve un JSON body con los
 *        tokens para que el cliente móvil los almacene.
 * \return CWS_OK con body enviado; CWS_ERR_NOTFOUND en credenciales
 *         inválidas (el llamador responde 401).
 */
int authorization_mobile_signin(cws_request_t* req, cws_response_t* res);

/* ------------------------------------------------------------------ */
/* acceso al estado de la petición                                    */
/* ------------------------------------------------------------------ */

const jwt_user_t* authorization_mobile_request_user(const cws_request_t* req);
bool authorization_mobile_request_authorized(const cws_request_t* req);

#ifdef __cplusplus
}
#endif

#endif /* AUTHORIZATION_MOBILE_H */
