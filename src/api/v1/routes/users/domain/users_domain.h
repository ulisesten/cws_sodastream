#ifndef USERS_DOMAIN_H
#define USERS_DOMAIN_H

#include "cws/cws.h"
#include "sql_eject.h"

/* Inicializa el acceso a datos del módulo de users (config + sql_eject).
 * Debe llamarse una vez tras cargar el .env del app. */
void users_domain_init(void);
void users_domain_shutdown(void);

/**
 * \brief users_new: registra un usuario (equivalente a users.domain.js).
 *
 * Ejecuta procUsersProc con tipoRegistro = PROC_USU_REGISTRAR (1) y los
 * datos ya hasheados por el service (correo y contraseña viajan como hash
 * Streebog-256). Los parámetros opcionales (apellidos, nombre) pueden ser
 * NULL — se envían como SQL NULL igual que undefined en la referencia.
 *
 * \return resultado malloc (liberar con sql_result_free), NULL en fallo.
 */
sql_result_t* users_domain_new(const char* usu_nombre,
                               const char* usu_ape_paterno,
                               const char* usu_ape_materno,
                               const char* hashed_correo,
                               const char* hashed_contrasena,
                               const char* usu_salt);

/**
 * \brief users_signin: consulta de usuario por su correo hasheado
 *        (procUsersCons, tipoConsulta = CONS_USU_SIGNIN = 1).
 * \return resultado malloc (liberar con sql_result_free), NULL en fallo.
 */
sql_result_t* users_domain_signin(const char* hashed_correo);

/**
 * \brief users_domain_set_public_id: asigna el identificador público de
 *        mensajería (cat_usuarios.usu_id_public) del usuario.
 *
 * Ejecuta procUsersProc con tipoRegistro = PROC_USU_ID_PUBLIC_AGREGAR (3).
 * El id lo genera el llamador (nanoid de 12). También estampa fecha_mod y
 * usuario_mod.
 *
 * \return resultado malloc (liberar con sql_result_free), NULL en fallo.
 */
sql_result_t* users_domain_set_public_id(int64_t usu_id,
                                         const char* public_id);

/**
 * \brief users_domain_by_public_id: busca un usuario por su id público
 *        (procUsersCons, tipoConsulta = 3).
 *
 * Devuelve filas con `usu_id, usu_nombre` (TOP 30). El llamador NO debe
 * exponer `usu_id`; es interno. \return resultado malloc, NULL en fallo.
 */
sql_result_t* users_domain_by_public_id(const char* public_id);

/**
 * \brief users_domain_list: lista de usuarios con mensajería habilitada
 *        (procUsersCons, tipoConsulta = 4), excluyendo `exclude_usu_id`.
 *
 * Devuelve `usu_id, usu_id_public, usu_nombre` ordenado por nombre. El
 * llamador NO debe exponer `usu_id`.
 */
sql_result_t* users_domain_list(int64_t exclude_usu_id);

#endif
