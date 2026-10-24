# AUTH — inicio de sesión y refresh para clientes (no navegador)

Guía del canal **Bearer** de `cws_sodastream` para clientes que **no** son el
navegador: apps Android/iOS, escritorio, CLI, integraciones y servicios.
No usa cookies, CSRF ni validación de IP; los tokens viajan en cabeceras.

> Los tokens son **opacos**: no son JWT estándar. Están cifrados/firmados con
> GOST (CFB + Streebog). **No** los parsees ni valides en el cliente; guárdalos
> y envíalos tal cual. Las librerías JWT habituales no sirven.

- **Base**: `https://cws.sodastream.fun`
- **Namespace**: `/api/v1/auth/mobile`
- **Auth de peticiones**: `Authorization: Bearer <access_token>`
- **Códigos**: `400` petición inválida · `401` credenciales/token inválidos ·
  `500` error interno/BD

---

## 1. Modelo de tokens

| Token | Vida (`.env`) | Uso | Se envía en |
|---|---|---|---|
| `access_token` | `ACCESS_TOKEN_EXPIRATION_MINUTES` (5 min) | autorizar peticiones | `Authorization: Bearer` |
| `refresh_token` | `REFRESH_TOKEN_EXPIRATION_DAYS` (180 días) | obtener un nuevo access | header de refresh (ver §3) |

- Ambos llevan `session_type = MOBILE_SESSION_TYPE` (2 por defecto).
- El access **no** está atado a IP (a diferencia del canal web).
- La revocación global (logout) rota `usu_salt` y **invalida ambos tokens**.

> ⚠️ `REFRESH_TOKEN_EXPIRATION_DAYS=180` es muy alto para un refresh sin
> rotación (ver §4). Recomendado: rotación + vida más corta (p. ej. 30 días).

---

## 2. Inicio de sesión

### `POST /api/v1/auth/mobile/signin`

```http
POST /api/v1/auth/mobile/signin
Content-Type: application/json

{"usu_correo":"user@test.local","usu_contrasena":"..."}
```

**200**

```json
{
  "access_token": "<opaco>",
  "refresh_token": "<opaco>",
  "token_type": "Bearer",
  "user": { "usu_id": 1017, "usu_nombre": "CWS", "usu_correo": "user@test.local" }
}
```

| Código | Caso |
|---|---|
| `400` | faltan `usu_correo`/`usu_contrasena` (`Campos requeridos: correo, contraseña`) |
| `401` | credenciales inválidas (`El usuario no fue autorizado o no existe.`) |
| `500` | servicio/BD no disponible |

**Qué hace el servidor**: normaliza el correo (trim+lower), lo busca por su
hash Streebog-256 en `procUsersCons`, verifica la contraseña (PBKDF2-Streebog)
y emite access + refresh. El refresh incluye en su payload `usu_correo`,
`usu_id`, `usu_nombre` y `usu_salt` (este último permite invalidar por logout).

**Qué debe hacer el cliente**:
1. Guardar ambos tokens en almacenamiento seguro (Keystore/Keychain), nunca en
   logs ni en almacenamiento plano.
2. Usar el access en cada petición.
3. Ante `401` de una petición normal, intentar **un** refresh y reintentar una
   vez; si el refresh también falla, volver al login.

---

## 3. Refresh

### Contrato actual (implementado)

```http
POST /api/v1/auth/mobile/refresh_token
X-Refresh-Token: <refresh_token>
```

**200**

```json
{ "access_token": "<nuevo>", "token_type": "Bearer" }
```

| Código | Caso |
|---|---|
| `401` | falta el header (`No credentials are present.`) |
| `401` | token inválido/expirado/revocado (`Authentication rejected.`) |
| `401` | `usu_salt` no coincide (logout/revocación) (`Error al iniciar sesión`) |

El servidor valida: `kind = refresh`, `session_type` móvil, firma, expiración y
`usu_salt` contra el DAO. **No** rota el refresh ni devuelve `expires_in` ni
`user`.

### Problemas del contrato actual

Comportamiento verificado contra el servidor:

| # | Problema | Evidencia |
|---|---|---|
| 1 | **No rota el refresh token** | el mismo `refresh_token` se puede usar N veces (200 en cada una) |
| 2 | **Vida de 180 días sin rotación** | un refresh filtrado sirve 6 meses y es reutilizable |
| 3 | **No devuelve `expires_in` / `refresh_expires_in`** | el cliente no sabe cuándo refrescar; los tokens son opacos (no puede leer `exp`) |
| 4 | **Header distinto al resto** | el refresh usa `X-Refresh-Token`, todo lo demás `Authorization: Bearer`; enviar el refresh como Bearer devuelve 401 `No credentials are present.` |
| 5 | **Sin detección de reuso ni revocación por sesión** | solo existe revocación global por `usu_salt` (logout cierra **todas** las sesiones/dispositivos) |
| 6 | **No devuelve `user`** | algunos clientes necesitan refrescar datos de perfil |
| 7 | **Errores inconsistentes** | salt/DAO pueden responder `401` o `500` según el caso |
| 8 | **Sin identificador de sesión** | imposible revocar un dispositivo o detectar replay de un refresh rotado |

### Flujo correcto (contrato objetivo)

```http
POST /api/v1/auth/mobile/refresh_token
Authorization: Bearer <refresh_token>
```

**200**

```json
{
  "access_token": "<nuevo>",
  "refresh_token": "<nuevo>",
  "token_type": "Bearer",
  "expires_in": 300,
  "refresh_expires_in": 2592000,
  "user": { "usu_id": 1017, "usu_nombre": "CWS", "usu_correo": "user@test.local" }
}
```

Reglas:

1. **Rotación obligatoria**: cada refresh emite un **nuevo** `refresh_token` y
   el anterior deja de servir.
2. **Detección de reuso**: si llega un refresh ya rotado → se asume robo y se
   revoca **toda la familia/sesión** (responder `401`).
3. **`expires_in`** en segundos (access) y **`refresh_expires_in`** (refresh),
   para que el cliente programe el refresh sin parsear el token.
4. **`user`** en la respuesta (o un `GET /me` inmediato).
5. **Revocación por sesión**: el logout normal revoca esa sesión; un logout
   global (p. ej. "cerrar en todos los dispositivos") rota `usu_salt`.
6. **Concurrencia**: si dos refresh simultáneos compiten, el servidor debe
   aceptar uno y rechazar el otro; el cliente debe hacer *single-flight*
   (una sola promesa de refresh compartida) y reintentar con el token más
   reciente. Ventana de gracia corta (p. ej. 30 s) opcional para no romper
   carreras de red.
7. **Compatibilidad**: mantener `X-Refresh-Token` como alias durante la
   transición, pero documentar `Authorization: Bearer` como canónico.

### Persistencia necesaria para rotar/revocar

El modelo actual es *stateless* (no hay registro del refresh), así que rotar sin
guardar estado no invalida el anterior. Para el flujo objetivo hace falta una
tabla de sesiones (esquema propuesto, convención `opr_`):

```sql
CREATE TABLE opr_usuarios_sesiones (
    ses_id              INT IDENTITY PRIMARY KEY,
    ses_id_public       VARCHAR(21)  NOT NULL,  -- nanoid expuesto al cliente
    usu_id              INT          NOT NULL,
    ses_refresh_hash    VARBINARY(64) NULL,     -- Streebog-256 del refresh vigente
    ses_prev_hash       VARBINARY(64) NULL,     -- para detectar reuso
    ses_exp             DATETIME     NOT NULL,
    ses_revoked         BIT          NOT NULL DEFAULT 0,
    ses_created         DATETIME     NOT NULL,
    ses_last_used       DATETIME     NULL,
    ses_ip              VARCHAR(45)  NULL,
    ses_user_agent      VARCHAR(255) NULL
);
CREATE UNIQUE INDEX ux_ses_id_public ON opr_usuarios_sesiones (ses_id_public);
CREATE INDEX ix_ses_usu ON opr_usuarios_sesiones (usu_id, ses_revoked);
CREATE INDEX ix_ses_hash ON opr_usuarios_sesiones (ses_refresh_hash);
```

- En signin: crear la fila y guardar `ses_refresh_hash = Streebog(refresh)`.
- En refresh: buscar por `ses_refresh_hash`; si coincide → rotar (nuevo refresh,
  actualizar `ses_refresh_hash`, `ses_prev_hash = anterior`).
- Si el hash presentado coincide con `ses_prev_hash` → reuso: revocar sesión.
- El access puede llevar el `ses_id_public` para auditar sin cambiar el formato
  del token (o añadirlo al payload en `jwt_core`).

---

## 4. Uso del access token

```http
GET /api/v1/auth/mobile/me
Authorization: Bearer <access_token>
```

**200** `{ "usu_id": 1017, "usu_nombre": "CWS", "usu_correo": "user@test.local" }`

- `401` sin token o token inválido/expirado/revocado.
- Flujo del cliente: petición → `401` → refresh (§3) → reintentar **una** vez →
  si vuelve a `401`, login.
- No reutilices un refresh para varias peticiones en paralelo: *single-flight*.

---

## 5. Logout y revocación

### `POST /api/v1/auth/mobile/logout`

```http
POST /api/v1/auth/mobile/logout
Authorization: Bearer <access_token>
```

**200** `{ "success": true, "error": 0, "msg": "Sesion cerrada" }`

Estado actual: rota `usu_salt` del usuario → **invalida todas** las sesiones y
dispositivos (access y refresh). Objetivo: revocar solo la sesión actual
(`ses_revoked = 1`) y ofrecer un logout global aparte.

Tras el logout, cualquier refresh devuelve `401 Error al iniciar sesión`.

---

## 6. WebSocket (mismo cliente)

El WS (`/ws/v1`, ver `WS_README.md`) **exige** el mismo access Bearer:

- El upgrade debe incluir `Authorization: Bearer <access_token>`; sin token
  válido el servidor responde `401` y no completa el handshake.
- El servidor valida firma + expiración + `session_type` + `usu_salt` contra
  el DAO (función `authorization_mobile_try_verify`, sin escribir HTTP).
- Identidad pública en `welcome`: `public_id` = `socket_id` (id de la
  conexión, 24 hex) y `nombre` = `usu_nombre`. **`usu_id` es interno y no se
  expone**.
- Mensajes: `message\n<texto>` (a `lobby`) o `message\n<room>\n<texto>`; el
  servidor emite `{"public_id","usu_nombre","msg","room","ts"}` a los miembros
  de la sala (incluido el emisor).
- **Refresh y reconexión**: el token se valida **solo en el upgrade**.
  Refrescar **no** afecta al socket abierto (no reconectes por refrescar); al
  reconectar debes enviar el access **más reciente**. Detalle, reglas y
  pitfalls de Android: `WS_README.md` → *Reconexión y refresh del access token*.
- Si el access caduca durante la conexión: pendiente cerrar con un código de
  aplicación (p. ej. `4001`) y que el cliente reconecte con un access
  refrescado.

> Ya no hay invitados: la conexión WS requiere autenticación.

---

## 7. Seguridad en el cliente

- Guardar tokens en **almacenamiento seguro** (Android Keystore / iOS Keychain /
  DPAPI / Secret Service). Nunca en `localStorage`, logs, crash reports ni
  ficheros planos.
- Usar **TLS** siempre; no aceptar certificados inválidos.
- No compartir tokens entre dispositivos; cada login = una sesión.
- Reloj del dispositivo: el servidor valida `exp`; un reloj muy desviado puede
  causar `401`. Sincronizar (NTP).
- Si el refresh falla repetidamente: borrar tokens y pedir login.
- Rotación de contraseña y "cerrar todas las sesiones": requieren revocación
  global (salt) en el objetivo.

---

## 8. Ejemplos

**Login**

```bash
curl -s -X POST https://cws.sodastream.fun/api/v1/auth/mobile/signin \
  -H 'Content-Type: application/json' \
  -d '{"usu_correo":"user@test.local","usu_contrasena":"..."}'
```

**Refresh (contrato actual)**

```bash
curl -s -X POST https://cws.sodastream.fun/api/v1/auth/mobile/refresh_token \
  -H "X-Refresh-Token: $REFRESH"
```

**Refresh (contrato objetivo)**

```bash
curl -s -X POST https://cws.sodastream.fun/api/v1/auth/mobile/refresh_token \
  -H "Authorization: Bearer $REFRESH"
```

**Petición autenticada**

```bash
curl -s https://cws.sodastream.fun/api/v1/auth/mobile/me \
  -H "Authorization: Bearer $ACCESS"
```

**Logout**

```bash
curl -s -X POST https://cws.sodastream.fun/api/v1/auth/mobile/logout \
  -H "Authorization: Bearer $ACCESS"
```

---

## 9. Checklist de implementación (servidor)

- [ ] Refresh acepta `Authorization: Bearer` (y mantiene `X-Refresh-Token`
      como alias deprecado).
- [ ] Rotar `refresh_token` en cada refresh.
- [ ] Tabla `opr_usuarios_sesiones` + `ses_refresh_hash`/`ses_prev_hash`.
- [ ] Detección de reuso → revocar sesión/familia y responder `401`.
- [ ] Devolver `expires_in`, `refresh_expires_in` y `user` en signin/refresh.
- [ ] Errores consistentes (`401` para credenciales/token; `500` solo fallo
      interno/BD).
- [ ] Logout por sesión + logout global (salt).
- [x] Integrar Bearer en el upgrade WebSocket (obligatorio; `usu_id` interno,
      `public_id` = socket_id, mensajes con `usu_nombre`).
- [ ] Tests: expiración, reuso, rotación, concurrencia, logout, salt.

## 10. Checklist de implementación (cliente)

- [ ] Guardar tokens en almacenamiento seguro.
- [ ] Añadir `Authorization: Bearer` a todas las peticiones.
- [ ] `401` → refresh → reintentar una vez → si falla, login.
- [ ] **Single-flight**: una sola llamada de refresh compartida.
- [ ] Persistir el **nuevo** refresh que devuelve el servidor (cuando exista
      rotación); si se pierde, obligar login.
- [ ] Programar refresh proactivo con `expires_in` (margen de seguridad, p. ej.
      30–60 s antes).
- [ ] Borrar tokens en logout y en `401` definitivo.
- [ ] No loggear tokens.
