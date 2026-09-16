# API — cws_sodastream

Servidor HTTP en C (framework `cws`) con acceso a SQL Server (ODBC),
autenticación web/móvil con tokens GOST y servidor estático HLS.

- **Base** (prod): `https://cws.sodastream.fun`
- **Puerto**: `.env` → `PORT`/`SERVER_PORT` (default `8080`)
- **Formato**: JSON. Errores con `{"success":false,"error":1,"msg":"...","status":N}`
  (web) o `{"success":false,"error":1,"msg":"..."}` (móvil).

## Índice

- [Generales](#generales)
- [Videos](#videos)
- [Auth web (cookies + CSRF)](#auth-web-cookies--csrf)
- [Auth móvil (Bearer)](#auth-móvil-bearer)
- [Estáticos HLS](#estáticos-hls)
- [Modelo de autenticación](#modelo-de-autenticación)
- [Configuración (.env)](#configuración-env)

---

## Generales

### `GET /`
Estado del servicio.
```json
{"service":"cws_sodastream","status":"running"}
```

### `GET /healthz`
Health check.
```json
{"status":"ok"}
```

### `GET /metrics`
Métricas en formato Prometheus (text/plain).

---

## Videos

Montados en `/api/v1/videos`. Requieren BD (`procCatVideosCons`).
Públicos (sin autenticación). Responden un **array JSON de filas** (las columnas
dependen de la consulta; p.ej. `vid_id`, `vid_id_public`, `vid_nombre`,
`vid_path`, `vid_descripcion`, `vid_fecha`, ...).

### `GET /api/v1/videos`
Listado completo (`tipoConsulta = 4`).

```bash
curl https://cws.sodastream.fun/api/v1/videos
```

### `GET /api/v1/videos/popular`
Videos más vistos (`tipoConsulta = 7`).

```bash
curl https://cws.sodastream.fun/api/v1/videos/popular
```

### `GET /api/v1/videos/:id`
Video por su id público (`tipoConsulta = 2`). `:id` = `vid_id_public`.

```bash
curl https://cws.sodastream.fun/api/v1/videos/SbW6CGLsXLf
```

| Código | Caso |
|---|---|
| 200 | OK (array, posiblemente vacío) |
| 500 | error de BD/procedimiento |
| 400 | falta `:id` |

---

## Auth web (cookies + CSRF)

Montados en `/api/v1/users`. Pensados para el navegador: sesión por **cookies**
y protección **CSRF** (header `X-CSRF-Token`). Además se valida la **IP** de
origen.

### `POST /api/v1/users/`
Registro de usuario.

**Body**
```json
{
  "usu_nombre": "CWS",
  "usu_ape_paterno": "Test",
  "usu_ape_materno": "User",
  "usu_correo": "user@test.local",
  "usu_contrasena": "TestPass123"
}
```
- `201` → `{"success":true,"error":0,"msg":"El usuario se creó correctamente"}`
- `400` → `{"success":false,"error":1,"msg":"Campos requeridos: correo, contraseña"}`
- `500` → error al registrar (p.ej. apellidos obligatorios en BD)

> El correo se guarda **hasheado** (Streebog-256) y la contraseña con
> **PBKDF2-HMAC-Streebog** (salt + pepper). Ver `core/password.c`.

### `POST /api/v1/users/signin`
Inicio de sesión. Responde OK y **4 cookies**:
`access_token` (HttpOnly), `csrf_token`, `refresh_token` (HttpOnly, Path
`/api/v1/users/refresh_token`), `refresh_csrf_token`.

**Body**
```json
{ "usu_correo": "user@test.local", "usu_contrasena": "TestPass123" }
```

**200**
```json
{
  "success": true,
  "error": 0,
  "msg": "Inicio de sesión exitoso",
  "data": { "usu_id": 1017, "usu_nombre": "CWS", "usu_correo": "user@test.local" }
}
```
- `400` faltan campos · `401` credenciales inválidas · `500` servicio no inicializado

### `POST /api/v1/users/refresh_token`
Renueva la sesión. Requiere cookie **`refresh_token`** y header
**`X-CSRF-Token`** (el `refresh_csrf_token`). Re-emite `access_token`,
`csrf_token` y `refresh_csrf_token`.

```bash
curl -i -X POST https://cws.sodastream.fun/api/v1/users/refresh_token \
  -b "refresh_token=<...>; refresh_csrf_token=<...>" \
  -H "X-CSRF-Token: <refresh_csrf_token>"
```
- `200` → `{"success":true,"error":0,"msg":"Sesion renovada"}` + `Set-Cookie`
- `401` credenciales/CSRF inválidos o IP distinta

### `GET /api/v1/users/me`
Datos del usuario autenticado. Requiere cookie **`access_token`** y header
**`X-CSRF-Token`** (`csrf_token`).

```bash
curl https://cws.sodastream.fun/api/v1/users/me \
  -b "access_token=<...>" -H "X-CSRF-Token: <csrf_token>"
```
**200**
```json
{ "usu_id": 1017, "usu_nombre": "CWS", "usu_correo": "user@test.local" }
```
- `401` → `"No credentials are present."` o `"Authentication rejected."`

---

## Auth móvil (Bearer)

Namespace propio `/api/v1/auth/mobile`. Sin cookies ni CSRF; pensado para
apps Android: los tokens van en el **body** y se envían como
`Authorization: Bearer <token>`. **No** valida IP.

### `POST /api/v1/auth/mobile/signin`
**Body** `{ "usu_correo": "...", "usu_contrasena": "..." }`

**200**
```json
{
  "access_token": "<...>",
  "refresh_token": "<...>",
  "token_type": "Bearer",
  "user": { "usu_id": 1017, "usu_nombre": "CWS", "usu_correo": "user@test.local" }
}
```
- `400` faltan campos · `401` credenciales inválidas · `500` no inicializado

### `POST /api/v1/auth/mobile/refresh_token`
Header **`X-Refresh-Token: <refresh_token>`**.

```bash
curl -X POST https://cws.sodastream.fun/api/v1/auth/mobile/refresh_token \
  -H "X-Refresh-Token: <refresh_token>"
```
**200** `{ "access_token": "<...>", "token_type": "Bearer" }`
- `401` token inválido

### `GET /api/v1/auth/mobile/me`
Header **`Authorization: Bearer <access_token>`**.

```bash
curl https://cws.sodastream.fun/api/v1/auth/mobile/me \
  -H "Authorization: Bearer <access_token>"
```
**200** `{ "usu_id": 1017, "usu_nombre": "CWS", "usu_correo": "user@test.local" }`
- `401` sin token / token inválido

---

## Estáticos HLS

### `GET /hls/videos/<ruta...>`
Archivos HLS desde `HLS_DIR` (`public/hls/videos` por defecto).

- Content-Type por extensión: `.m3u8` → `application/vnd.apple.mpegurl`
  (`Cache-Control: no-cache`), `.ts` → `video/mp2t`
  (`Cache-Control: public, max-age=31536000`), etc.
- Protección **anti-traversal** y **dotfiles** (404). Symlinks fuera de la raíz
  → 404.

```bash
curl -i https://cws.sodastream.fun/hls/videos/playlist.m3u8
```

---

## Modelo de autenticación

- **Tokens GOST** (cifrado reversible CFB 28147-89 + firma Streebog-256 del
  payload completo). `session_type` distinto por canal (web=1, móvil=2):
  un token móvil no vale en web ni viceversa.
- **Web**: `access_token` (vida `ACCESS_TOKEN_EXPIRATION_MINUTES`, def. 15 min)
  + `refresh_token` (`REFRESH_TOKEN_EXPIRATION_DAYS`, def. 7 días).
  Cada request valida además la **IP** de origen y el **salt** contra la BD.
- **Móvil**: mismos tokens, sin IP ni CSRF.
- **Contraseñas**: PBKDF2-HMAC-Streebog con salt aleatorio (16 B) y
  `PASSWORD_PEPPER` opcional; iteraciones `PASSWORD_KDF_ITERATIONS`
  (def. 100000). `password_verify` acepta hashes legacy (Streebog-256) y avisa
  para rehash.

### CORS

Si el front está en otro origen (p. ej. `https://video.sodastream.fun`),
configura `CORS_ORIGINS` (lista separada por comas). Para el flujo por cookies
se responde el `Origin` + `Access-Control-Allow-Credentials: true`
(y `X-CSRF-Token` en `Allow-Headers`). Las cookies usan
`COOKIE_DOMAIN` para compartirse entre subdominios.

---

## Configuración (.env)

```ini
# Servidor
PORT=8080
HLS_DIR=public/hls/videos
CORS_ORIGINS=https://video.sodastream.fun
COOKIE_DOMAIN=.sodastream.fun

# BD (SQL Server / ODBC)
DB_DRIVER=ODBC Driver 17 for SQL Server
DB_SERVER=192.168.1.78
DB_PORT=1433
DB_USER=...
DB_PASSWORD=...
DB_DATABASE=soda_stream
DB_ENCRYPT=true
DB_TRUST_CERTIFICATE=true

# Auth
SECRET_KEY=<clave JWT/IV>
X_VECTOR=0x..,0x..,0x..,0x..,0x..,0x..,0x..,0x..   # 8 palabras hex (clave GOST)
ACCESS_TOKEN_EXPIRATION_MINUTES=15
REFRESH_TOKEN_EXPIRATION_DAYS=7
WEB_SESSION_TYPE=1
MOBILE_SESSION_TYPE=2
PASSWORD_KDF_ITERATIONS=100000
PASSWORD_PEPPER=<secreto del servidor>
NODE_ENV=production                                # cookies Secure
```
