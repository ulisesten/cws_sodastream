/*
 * users_routes.c — rutas y service del módulo de users.
 *
 * Espejo en C de reference/users/routes.js + service/users.service.js +
 * dto/users.dto.js (dto inline). Los handlers usan el servicio global de
 * authorization (signin) y users_domain (DAO).
 */

#define _POSIX_C_SOURCE 200809L

#include "users_routes.h"
#include "domain/users_domain.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <gost/gost.h>

#include "authorization.h"
#include "jwt.h"

/* ------------------------------------------------------------------ */
/* helpers de respuesta (reject + json de éxito, y dto)                 */
/* ------------------------------------------------------------------ */

/* Copia `s` a `out` con escapes JSON mínimos (", \ y controles).
 * Devuelve la longitud escrita. */
static size_t json_escape_to(char* out, size_t cap, const char* s) {
    size_t o = 0;
    for (const unsigned char* p = (const unsigned char*)(s ? s : "");
         *p && o + 2 < cap; p++) {
        switch (*p) {
            case '"':  out[o++] = '\\'; out[o++] = '"'; break;
            case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
            case '\n': out[o++] = '\\'; out[o++] = 'n'; break;
            case '\r': out[o++] = '\\'; out[o++] = 'r'; break;
            case '\t': out[o++] = '\\'; out[o++] = 't'; break;
            default:
                if (*p < 0x20) {
                    int n = snprintf(out + o, cap - o, "\\u%04x", *p);
                    if (n < 0 || (size_t)n >= cap - o) return o;
                    o += (size_t)n;
                } else {
                    out[o++] = (char)*p;
                }
                break;
        }
    }
    out[o] = '\0';
    return o;
}

static void send_json(cws_response_t* res, const char* body) {
    cws_response_body(res, body, strlen(body), CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

/* reject(res, status, msg): {"success":false,"error":1,"msg":"..."} */
static void reject_json(cws_response_t* res, int status, const char* msg) {
    char body[512];
    int n = snprintf(body, sizeof(body),
                     "{\"success\":false,\"error\":1,\"msg\":\"");
    if (n < 0) n = 0;
    size_t len = (size_t)n;
    len += json_escape_to(body + len, sizeof(body) - len - 8, msg);
    n = snprintf(body + len, sizeof(body) - len, "\"}");
    if (n < 0) len = 0;
    else len += (size_t)n;
    cws_response_status(res, status);
    cws_response_body(res, body, len, CWS_MT_APPLICATION_JSON);
    cws_response_send(res);
}

/* ------------------------------------------------------------------ */
/* nanoid (usu_salt)                                                    */
/* ------------------------------------------------------------------ */

/** Alfabeto default de nanoid (A-Za-z0-9_-): 64 símbolos = 6 bits/byte. */
static const char nanoid_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";

/** nanoid(size): usa gost_rng_system; byte & 0x3F es uniforme (64 | 256). */
static char* nanoid(size_t len) {
    uint8_t raw[256];
    if (len == 0 || len > sizeof(raw)) return NULL;
    if (gost_rng_system(NULL, raw, len) != GOST_OK) return NULL;
    char* out = (char*)malloc(len + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++)
        out[i] = nanoid_alphabet[raw[i] & 0x3F];
    out[len] = '\0';
    return out;
}

/* ------------------------------------------------------------------ */
/* POST /signin — login                                                 */
/* ------------------------------------------------------------------ */

/*
 * user_signin: valida el body, delega en authorization_signin (que verifica
 * la contraseña y emite las 4 cookies) y responde con el dto del usuario:
 * {"usu_id","usu_nombre","usu_correo"}.
 */
static CWS_HANDLER(user_signin_handler) {
    char* body = NULL;
    char* email = NULL;
    char* password = NULL;
    jwt_user_t auth_user;
    memset(&auth_user, 0, sizeof(auth_user));

    /* Cuerpo JSON NUL-terminado (req->body no lo está). */
    if (req->body && req->body_len > 0 && req->body_len <= 8192) {
        body = (char*)malloc(req->body_len + 1);
        if (body) {
            memcpy(body, req->body, req->body_len);
            body[req->body_len] = '\0';
        }
    }
    if (body) {
        jwt_json_get_string(body, "usu_correo", &email);
        jwt_json_get_string(body, "usu_contrasena", &password);
    }

    if (!email || !*email || !password || !*password) {
        reject_json(res, 400, "Campos requeridos: correo, contraseña");
        goto done;
    }

    int rc = authorization_signin(req, res, &auth_user);
    if (rc != CWS_OK) {
        cws_response_status(res, 401);
        send_json(res, "{\"success\":false,\"error\":1,"
                       "\"msg\":\"El usuario no fue autorizado o no "
                       "existe.\",\"status\":401}");
        goto done;
    }

    /* usersDto.signinToResponse(auth_service_response) */
    {
        char nombre[256], correo[256];
        json_escape_to(nombre, sizeof(nombre), auth_user.usu_nombre);
        json_escape_to(correo, sizeof(correo), auth_user.usu_correo);
        char body_json[768];
        int n = snprintf(body_json, sizeof(body_json),
                         "{\"success\":true,\"error\":0,"
                         "\"msg\":\"Inicio de sesión exitoso\",\"data\":{"
                         "\"usu_id\":%lld,\"usu_nombre\":\"%s\","
                         "\"usu_correo\":\"%s\"}}",
                         (long long)auth_user.usu_id, nombre, correo);
        if (n < 0 || (size_t)n >= sizeof(body_json)) {
            reject_json(res, 500, "Error al registrar usuario");
        } else {
            cws_response_status(res, 200);
            send_json(res, body_json);
        }
    }

done:
    authorization_user_free(&auth_user);
    free(body);
    free(email);
    free(password);
}

/* ------------------------------------------------------------------ */
/* POST / — registro de usuario                                         */
/* ------------------------------------------------------------------ */

/*
 * user_new: valida el body, hashea correo (normalizado) y contraseña
 * (trimmed), genera usu_salt con nanoid(10) y registra vía procUsersProc.
 * Responde 201 {"success":true,...} o 400/500 reject.
 */
static CWS_HANDLER(user_new_handler) {
    char* body = NULL;
    char* usu_nombre = NULL;
    char* ape_paterno = NULL;
    char* ape_materno = NULL;
    char* email = NULL;
    char* password = NULL;
    char* hashed_email = NULL;
    char* hashed_pass = NULL;
    char* salt = NULL;
    sql_result_t* dao = NULL;

    if (req->body && req->body_len > 0 && req->body_len <= 8192) {
        body = (char*)malloc(req->body_len + 1);
        if (body) {
            memcpy(body, req->body, req->body_len);
            body[req->body_len] = '\0';
        }
    }
    if (body) {
        jwt_json_get_string(body, "usu_nombre", &usu_nombre);
        jwt_json_get_string(body, "usu_ape_paterno", &ape_paterno);
        jwt_json_get_string(body, "usu_ape_materno", &ape_materno);
        jwt_json_get_string(body, "usu_correo", &email);
        jwt_json_get_string(body, "usu_contrasena", &password);
    }

    if (!email || !*email || !password || !*password) {
        reject_json(res, 400, "Campos requeridos: correo, contraseña");
        goto done;
    }

    /* normalize_email: trim + lowercase. */
    for (char* p = email; *p; p++) *p = (char)tolower((unsigned char)*p);

    hashed_email = jwt_hash_hex(email);

    /* hashed_contrasena = hash(contrasena.trim()). */
    char* pw = password;
    while (*pw == ' ' || *pw == '\t') pw++;
    size_t pw_len = strlen(pw);
    while (pw_len > 0 && (pw[pw_len - 1] == ' ' || pw[pw_len - 1] == '\t'))
        pw[--pw_len] = '\0';
    char save = pw[pw_len];
    pw[pw_len] = '\0';
    hashed_pass = jwt_hash_hex(pw);
    pw[pw_len] = save;
    if (!hashed_email || !hashed_pass) {
        reject_json(res, 500, "Error al registrar usuario");
        goto done;
    }

    salt = nanoid(10);
    if (!salt) {
        reject_json(res, 500, "Error al registrar usuario");
        goto done;
    }

    dao = users_domain_new(usu_nombre, ape_paterno, ape_materno,
                           hashed_email, hashed_pass, salt);
    if (!dao) {
        reject_json(res, 500, "Error al registrar usuario");
        goto done;
    }

    cws_response_status(res, 201);
    send_json(res,
              "{\"success\":true,\"error\":0,"
              "\"msg\":\"El usuario se creó correctamente\"}");

done:
    sql_result_free(dao);
    free(body);
    free(usu_nombre);
    free(ape_paterno);
    free(ape_materno);
    free(email);
    free(password);
    free(hashed_email);
    free(hashed_pass);
    free(salt);
}

/* ------------------------------------------------------------------ */
/* router                                                              */
/* ------------------------------------------------------------------ */

cws_router_t* users_routes(void) {
    cws_router_t* r = cws_router_new();
    if (!r) return NULL;

    cws_router_add(r, CWS_M_POST, "/signin", user_signin_handler);
    cws_router_add(r, CWS_M_POST, "/", user_new_handler);

    return r;
}
