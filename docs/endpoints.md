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
- [App](#app)
- [Auth web (cookies + CSRF)](#auth-web-cookies--csrf)
- [Auth móvil (Bearer)](#auth-móvil-bearer)
- [Mensajería](#mensajería)
- [Llamadas (señalización 1:1)](#llamadas-señalización-11)
- [Integraciones](#integraciones)
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
Públicos (sin autenticación), salvo `POST /external`. Responden un **array JSON
de filas** (las columnas dependen de la consulta; p.ej. `vid_id`,
`vid_id_public`, `vid_nombre`, `vid_path`, `vid_descripcion`, `vid_fecha`, ...).

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

### `GET /api/v1/videos/:id/series/relacionados`
Videos de la serie del video (`tipoConsulta = 1`, `vid_id = :id` **numérico**).
Alimenta la barra de relacionados del reproductor.

```bash
curl https://cws.sodastream.fun/api/v1/videos/4/series/relacionados
```
**200**
```json
{ "msg": "Se obtuvieron los videos relacionados correctamente.",
  "success": true, "error": 0, "data": [ /* videos de la serie */ ] }
```
Sin resultados: `{ "msg": "Sin resultados.", "success": true, "error": 0, "data": null }`.
- `400` `:id` no numérico · `500` error de BD

### `PUT /api/v1/videos/:id/views`
Incrementa el contador de vistas (`procCatVideosProc`,
`tipoRegistro = "CAT_VIDEOS_VIEW"`, `vid_id = :id` **numérico**). `:id` = `vid_id`.

```bash
curl -X PUT https://cws.sodastream.fun/api/v1/videos/1/views
```
**200** `{ "msg": "", "success": "true", "error": 0 }`
- `400` `:id` no numérico · `500` error de BD

### `GET /api/v1/videos/thumbnails/:name`
Sirve la imagen de la miniatura (`tipoConsulta = 3`). `:name` es
`<thu_id_public>` o `<thu_id_public>.<ext>` (el último punto separa la
extensión). Los archivos se resuelven en **`API_NAS`** por el nombre base del
`thu_path` de la BD, con fallback de extensiones `jpg,jpeg,png,gif,webp`.
Responde la imagen vía `sendfile` con su `Content-Type`.

```bash
curl -O https://cws.sodastream.fun/api/v1/videos/thumbnails/OPtESlF0bjg.jpg
```
| Código | Caso |
|---|---|
| 200 | imagen (Content-Type por extensión) |
| 200 | JSON `{"msg":"Ocurrió un error al consultar la imagen.","success":false,"error":1}` si la miniatura no existe en BD |
| 404 | JSON `{"msg":"Imagen no encontrada","success":false,"error":1}` si no hay archivo |

### `POST /api/v1/videos/external`
**Protegido** (cookie `access_token` + `x-csrf-token` + IP; ver
[Auth web](#auth-web-cookies--csrf)). Es la excepción del módulo. Registra un
video de fuente externa (URL m3u8 + miniatura existente, sin subida física) a
nombre del usuario autenticado. `procCatVideosProc`,
`tipoRegistro = "CAT_VIDEOS_EXTERNAL_INS"`; genera `vid_id_public` con nanoid
(`PUBLIC_ID_LENGTH`).

Body JSON:

| Campo | Tipo | Requerido | Notas |
|---|---|---|---|
| `vid_nombre` | string | sí | |
| `vid_path` | string | sí | URL del `.m3u8` externo |
| `vid_id_thumbnail` | int o string (`"1"`) | no | `thu_id` de `cat_videos_thumbnails` |
| `vid_descripcion` | string | no | la rama externa del SP no la almacena |
| `vid_tags` | string | no | |

```bash
curl -X POST https://cws.sodastream.fun/api/v1/videos/external \
  -H 'Content-Type: application/json' \
  -b cookies.txt -H "X-CSRF-Token: $CSRF" \
  -d '{"vid_nombre":"Mi video","vid_path":"https://cdn.example.com/x.m3u8","vid_id_thumbnail":1}'
```
**200**
```json
{ "msg": "Video Externo subido correctamente.", "success": "true", "error": 0,
  "data": { "vid_id": 8, "vid_thumbnail": "SbW6CGLsXLf" } }
```
`data.vid_thumbnail` es el `thu_id_public` de la miniatura (`null` si no se pasó
`vid_id_thumbnail` o no existe).
- `400` falta `vid_nombre`/`vid_path` · `401` sin sesión válida · `500` error de BD

---

## App

Montados en `/api/v1/app`. Públicos (sin autenticación).

### `GET /api/v1/app/releases`
Versiones/releases de la aplicación (`procCatAppReleasesCons`,
`tipoConsulta = 1`).

```bash
curl https://cws.sodastream.fun/api/v1/app/releases
```
**200**
```json
{ "success": true, "error": 0, "msg": "Releases obtenidos exitosamente",
  "data": [ { "rel_id": 1, "rel_version": "0.1", "rel_path": "…",
              "rel_type": "TV", "rel_date": "…", "rel_description": "…" } ] }
```
- `404` `{"success":false,"error":1,"msg":"No se encontraron releases","data":[]}`
- `500` error de BD

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

### `POST /api/v1/users/logout`
Cierra la sesión. Requiere cookie **`access_token`** y header
**`X-CSRF-Token`**. Rota el `usu_salt` del usuario (revoca **todos** sus
tokens) y limpia `access_token`/`csrf_token`.

```bash
curl -i -X POST https://cws.sodastream.fun/api/v1/users/logout \
  -b "access_token=<...>" -H "X-CSRF-Token: <csrf_token>"
```
- `200` → `{"success":true,"error":0,"msg":"Sesion cerrada"}` + `Set-Cookie` de borrado
- `401` credenciales/CSRF inválidos

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

> Estado actual y flujo completo (login, refresh, rotación, revocación) para
> clientes: ver **`AUTH_README.md`**. El refresh actual no rota el token ni
> devuelve `expires_in`/`refresh_expires_in` (ver defectos y contrato objetivo).

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

### Dispositivos (notificaciones)

Requiere Bearer. Guarda uno o varios dispositivos por usuario para
notificaciones (FCM/APNs futuro).

- `POST /api/v1/users/device` body
  `{ "device_id": "...", "plataforma": 1, "nombre": "...",
  "push_token": "..." }` (device 1-255; plataforma 1 Android, 2 Linux,
  3 Windows, 4 iOS, 5 web; nombre ≤100; push_token ≤512).
- `GET /api/v1/users/device` → `device_id` (primero) + `devices`:
  `[{device_id, plataforma, nombre, activo, ultimo_uso}]`.
- `DELETE /api/v1/users/device` body `{ "device_id": "..." }` → elimina ese
  dispositivo; sin `device_id` desactiva todos (logout-all).

Requiere aplicar `db_sodastream/usuarios_devices.sql`
(`opr_usuarios_devices` + `procUsersDevicesProc/Cons`). La tabla vieja
`usuarios_device_id.sql` queda solo por compatibilidad.

El flujo completo de push (FCM, token, HTTP v1, full-screen intent) está en
`docs/fcm_setup.md`.

### `POST /api/v1/auth/mobile/logout`
Header **`Authorization: Bearer <access_token>`**. Rota el `usu_salt` del
usuario, revocando **todos** sus tokens (access y refresh).

```bash
curl -X POST https://cws.sodastream.fun/api/v1/auth/mobile/logout \
  -H "Authorization: Bearer <access_token>"
```
**200** `{ "success": true, "error": 0, "msg": "Sesion cerrada" }`
- `401` sin token / token inválido

---

## Mensajería

Montado en `/api/v1/messaging`. Requiere access token móvil (**Bearer**).

> En la BD, `opr_chat_mensajes.msg_tipo` **ya existe y es `TINYINT`**
> (`0`=texto, `1`=sticker). La API usa `"tipo":"text"|"sticker"` y el servidor
> lo mapea a `0/1`; no se agrega una columna nueva.

### `POST /api/v1/messaging/setup`
Habilita la mensajería instantánea del usuario autenticado: genera un
identificador público estilo **nanoid** (12 caracteres, `A-Za-z0-9_-`), lo
guarda en `cat_usuarios.usu_id_public` (`procUsersProc`,
`tipoRegistro = 3`) y lo devuelve. Este id será la identidad pública
persistente del usuario para el chat 1:1.

```bash
curl -X POST -H "Authorization: Bearer <access_token>" \
  https://cws.sodastream.fun/api/v1/messaging/setup
```
**201**
```json
{"success":true,"error":0,"msg":"Mensajeria instantanea habilitada",
 "data":{"usu_id_public":"VN2qxVIsbz5t","ws_path":"/ws/v1"}}
```
- `401` sin Bearer válido.
- `500` si falla la BD/RNG.
- Cada llamada **regenera** el id: el cliente debe guardarlo y no repetir la
  llamada salvo que quiera rotarlo.
- `usu_id` es interno y **no** se devuelve.

### `GET /api/v1/messaging/users`
Lista de usuarios con mensajería habilitada (excluye al propio). Requiere
**Bearer**. El cliente puede **filtrar la lista localmente** para buscar.

```bash
curl -H "Authorization: Bearer <access_token>" \
  https://cws.sodastream.fun/api/v1/messaging/users
```
**200**
```json
{"success":true,"error":0,"msg":"Usuarios obtenidos",
 "data":[{"usu_id_public":"AB12cd34EF56","usu_nombre":"KDF2"}]}
```
- Solo usuarios con `usu_id_public` de 12; **nunca** se devuelve `usu_id`.
- `401` sin Bearer válido · `500` error de BD.

> `procUsersCons` `tipoConsulta = 4` (excluye `@usu_id`).

### `GET /api/v1/messaging/users/:public_id`
Busca un usuario por su id público de mensajería (nanoid de 12). Requiere
**Bearer**. Devuelve solo datos públicos; **nunca** `usu_id`.

```bash
curl -H "Authorization: Bearer <access_token>" \
  https://cws.sodastream.fun/api/v1/messaging/users/JV4y-3-Bkdia
```
**200**
```json
{"success":true,"error":0,"msg":"Usuario encontrado",
 "data":{"usu_id_public":"JV4y-3-Bkdia","usu_nombre":"KDF2"}}
```
- `400` id con formato inválido (deben ser 12 caracteres `A-Za-z0-9_-`).
- `404` no existe.
- `401` sin Bearer válido.
- `500` error de BD.

> Ejecuta `procUsersCons` con `tipoConsulta = 3` (`@usu_id_public`).

### `GET /api/v1/messaging/conversations/:public_id/messages`
Historial de la conversación con el contacto `:public_id` (Bearer). Orden:
**más nuevo primero**. El servidor resuelve tu identidad desde el token.

Query: `before=<msg_id>` (opcional), `limit` (default 30, máx 100).

```bash
curl -H "Authorization: Bearer <access_token>" \
  "https://cws.sodastream.fun/api/v1/messaging/conversations/JV4y-3-Bkdia/messages?limit=30"
```
**200**
```json
{"success":true,"error":0,"msg":"Mensajes obtenidos",
 "data":{"messages":[
   {"msg_id":124,"from":"AB12cd34EF56","es_mio":false,
    "tipo":"text","sticker_id":null,"texto":"hola",
    "fecha":"2026-11-13T18:05:00","reply_to":null,"leido":false}],
  "next_before":124}}
```
- `400` `before`/`limit` inválidos · `404` contacto no existe · `401` sin token.
- `next_before`: cursor para la página siguiente (`null` si no hay más).
- `from` es el `usu_id_public` del emisor; `es_mio` lo calcula el servidor.

### `POST /api/v1/messaging/conversations/:public_id/messages`
Envía un mensaje al contacto (Bearer). Persiste en `opr_chat_mensajes` y
empuja el evento WS `dm` al receptor y a tus otros dispositivos.

```bash
curl -X POST -H "Authorization: Bearer <access_token>" \
  -H "Content-Type: application/json" \
  -d '{"texto":"hola","reply_to":123}' \
  https://cws.sodastream.fun/api/v1/messaging/conversations/JV4y-3-Bkdia/messages
```
**201**
```json
{"success":true,"error":0,"msg":"Mensaje enviado",
 "data":{"msg_id":125,"from":"VN2qxVIsbz5t","es_mio":true,
         "texto":"hola","fecha":"2026-11-13T18:06:00",
         "reply_to":123,"leido":false}}
```
- Texto: `{"texto":"hola","reply_to":123}` (≤4096 bytes UTF-8).
- Sticker: `{"tipo":"sticker","sticker_id":"<64 hex>","reply_to":null}`
  (sin `texto`); el sticker debe estar registrado.
- `400` mensaje/reply/tipo inválido · `404` contacto o sticker no existe ·
  `401` sin token.

### `POST /api/v1/messaging/conversations/:public_id/read`
Marca como leídos los mensajes del contacto hasta `hasta` (opcional; sin él,
todos). Bearer.

```bash
curl -X POST -H "Authorization: Bearer <access_token>" \
  -H "Content-Type: application/json" -d '{"hasta":124}' \
  https://cws.sodastream.fun/api/v1/messaging/conversations/JV4y-3-Bkdia/read
```
**200** `{"success":true,"error":0,"msg":"Mensajes marcados como leidos","data":{"actualizados":1}}`

> Chat 1:1: `procChatMensajesCons` (historial) y `procChatMensajesProc`
> (insertar/marcar leído). Requieren la tabla `opr_chat_mensajes` y los SP de
> `db_sodastream/`.

### Stickers

Content-addressed: `sti_id` = **Streebog-256 hex (64)** de los bytes del
`.webp`; el archivo vive en `STICKERS_DIR/<sti_id>.webp` (ruta **absoluta** en
`.env`, igual que `HLS_DIR`/`API_NAS`). Todas requieren Bearer.

#### `POST /api/v1/messaging/stickers`
Body = bytes WebP (`Content-Type: image/webp`), 512×512, ≤1 MiB. El servidor
crea/valida `STICKERS_DIR`, calcula el hash y registra (idempotente);
`X-Sticker-Id` opcional (si viene, debe coincidir). **201** nuevo / **200**
existente:
```json
{"success":true,"error":0,"msg":"Sticker registrado","data":{
  "sticker_id":"<64 hex>","mime":"image/webp","bytes":554,
  "width":512,"height":512,"animated":false,"exists":false,
  "url":"/api/v1/messaging/stickers/<64 hex>/file"}}
```
`400` WebP/dimensiones/`X-Sticker-Id` · `413` >1 MiB · `415` mime.

#### `GET /api/v1/messaging/stickers/:sticker_id`
Metadatos (mismo `data`, sin `exists`). `404` no existe.

#### `GET /api/v1/messaging/stickers/:sticker_id/file`
Archivo `image/webp` con `Cache-Control: private, max-age=31536000, immutable`
y `ETag: "<id>"` (304 con `If-None-Match`). `404` si no está.

#### `POST /api/v1/messaging/stickers/check`
```json
{"contact_public_id":"<opcional>","sticker_ids":["<64 hex>"]}
```
**200** `{"data":{"registered":[],"missing":[],"contact_has":[],"contact_missing":[]}}`
(sin `contact_public_id` solo `registered`/`missing`).

#### `POST /api/v1/messaging/stickers/sync`
```json
{"sticker_ids":["<64 hex>"]}
```
Reemplaza el inventario del usuario. **200** `{"data":{"synced":N}}`.

> SP `procChatStickersProc`/`Cons`, columnas `msg_tipo`/`msg_sticker_id` y
> `opr_chat_stickers`/`opr_usuarios_sticker` en `db_sodastream/`; detalle en
> [`chat_stickers.md`](chat_stickers.md).

> No hay endpoint de "lista de conversaciones" (inbox): el cliente usa
> `GET /api/v1/messaging/users` + caché local (ver la guía).

Guía de integración del cliente para el flujo 1:1 (identificadores, historial,
envío y push `dm`): [`chat_1a1_cliente.md`](chat_1a1_cliente.md).

---

## Llamadas (señalización 1:1)

Requiere Bearer. El alta/aceptación de la llamada y el aviso Telegram siguen
en `/api/v1/integrations/telegram` (puente de notificación); esta sección es
la señalización WebRTC.

- `GET /api/v1/calls/:call_id` → estado, rol, peer y `peer_ip` informativo.
- `POST /api/v1/calls/identity` → `{ "device_id": "...", "identity_key":
  "<base64>" }`; guarda la clave pública del dispositivo.
- `GET /api/v1/calls/identity/:usu_id_public` → claves públicas del usuario
  (`identities: [{device_id, identity_key}]`).
- `GET /api/v1/calls/:call_id/ice` → `ice_servers` STUN/TURN con credenciales
  temporales (`expires_in`).
- `POST /api/v1/calls/:call_id/sdp`
  `{ "type": "offer|answer", "sdp": "..." }` → releva `call.sdp` al peer.
- `POST /api/v1/calls/:call_id/ice`
  `{ "candidate": "...", "sdpMid": "...", "sdpMLineIndex": 0,
  "usernameFragment": "..." }` → releva `call.ice` al peer.

SDP/ICE solo con la llamada `accepted` (`409` si no). El servidor no maneja
media ni descifra. `CALL_E2EE_REQUIRED=true` exige `e2ee` en notify/accept
(`426 E2EE_UNSUPPORTED`); `CALL_ID_SERVER=true` hace que el servidor genere
el `call_id`. Ver `docs/telegram_client.md`.

**Estado verificado (2026-11-16):**

- Audio 1:1 **Wi-Fi ↔ Wi-Fi**: funciona.
- Audio 1:1 **Wi-Fi ↔ datos móviles**: funciona (ruta P2P o relay TURN).
- **Vídeo 1:1** con un extremo en datos móviles: funciona.
- `/calls/:id/ice` devuelve STUN + TURN autenticado (usuario y credencial
  temporales) con relay **público**; probado `turn:` en **UDP y TCP 3478**.
- `turns:…:5349` sigue cerrado y `stun.sodastream.fun` sin registro A. Usa
  `stun:turn.sodastream.fun:3478` o corrige el despliegue; `turns` en 5349
  no está disponible todavía.
- El cliente añade al SDP `a=x-gost-e2ee:v1:<base64(x||y)>` con su clave
  efímera; el servidor lo releva tal cual. Sin ese atributo el cliente
  cierra la llamada (fail-closed). Detalle en `calls_e2ee.md`.

Si `ice_servers` viene vacío, `data.warning` lo explica: faltan
`STUN_URLS`/`TURN_URLS`/`TURN_SECRET` en `.env`. Despliegue en
`docs/turn_deploy.md`. Sin TURN solo funcionan rutas
P2P directas/LAN; para NAT simétrico hace falta TURN. El `TURN_SECRET` debe
coincidir con `static-auth-secret` de coturn.

---

## Integraciones

Montado en `/api/v1/integrations`. Ver `docs/telegram_bot.md` para el flujo
completo y la configuración.

### Telegram (Bearer)

- `POST /api/v1/integrations/telegram/link` → `{ code, url, expires_in }`.
  Abrir `url` en Telegram y pulsar Start.
- `GET /api/v1/integrations/telegram/status` → `{ linked, telegram_username,
  telegram_name }`.
- `POST /api/v1/integrations/telegram/unlink` → borra el vínculo.
- `POST /api/v1/integrations/telegram/notify` body
  `{ "usu_id_public": "...", "tipo": "audio|video|walkie",
  "e2ee": {"v":1,"algs":["gost"],"confirm":true},
  "ip": "...", "telefono": "..." }` → estado ringing en memoria, mapea
  IP/teléfono del solicitante, emite WSS `call.incoming` y, si no hay WS o
  `TELEGRAM_NOTIFY_ALWAYS=1`, manda Telegram con `cws://call/<call_id>`.
  El servidor genera el `call_id` y lo devuelve; el cliente no debe inventarlo.
- `POST /api/v1/integrations/telegram/accept` (receptor) body
  `{ "call_id": "...", "e2ee": {"v":1,"algs":["gost"],"confirm":true},
  "ip": "...", "telefono": "..." }` → valida, mapea IP/teléfono del receptor
  y emite WSS `call.accepted` al solicitante.
- `POST /api/v1/integrations/telegram/reject` `{ "call_id": "..." }` →
  `call.rejected`.
- `POST /api/v1/integrations/telegram/end` `{ "call_id": "..." }` →
  `call.ended` al otro extremo.
- `GET /api/v1/integrations/telegram/pending` → llamadas ringing/aceptadas.

La IP se toma de `X-Forwarded-For`/`X-Real-IP` y, si no existe, del body.

### Sistema externo (HMAC)

Headers: `X-Integration-Timestamp`, `X-Integration-Nonce`,
`X-Integration-Signature` = hex HMAC-Streebog-256 de
`timestamp + "\n" + nonce + "\n" + body`. Ventana ±300 s; nonce no reutilizable.

- `POST /api/v1/integrations/external/link`
  `{ "usu_id_public": "...", "telefono": "...", "ip": "...", "id_externo": "..." }`
  (normalmente los mapeos ya los crean `/telegram/notify` y `/telegram/accept`).
- `POST /api/v1/integrations/external/notify`
  `{ "telefono" | "ip" | "id_externo" | "usu_id_public": "...", "call_id": "...",
  "tipo": "audio|video|walkie", "nombre": "..." }`

> Sin FCM/APNs: con la app abierta el aviso va por WSS; con la app cerrada el
> usuario recibe Telegram y toca el deep link. Requiere aplicar
> `db_sodastream/video_llamadas_integraciones.sql` con un usuario con permisos DDL.
> Guía de cliente: `docs/telegram_client.md`.

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
- **Logout / revocación**: `POST /logout` (web y móvil) rota el `usu_salt` del
  usuario; como cada request compara el salt del token con el de la BD, todos
  los tokens previos (access y refresh) quedan invalidados de inmediato.
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
API_NAS=/ruta/nas/images/videos
CORS_ORIGINS=https://midominio.com
COOKIE_DOMAIN=.midominio.com

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

# Telegram / integración externa (ver docs/telegram_bot.md)
TELEGRAM_BOT_TOKEN=123456:ABC...
TELEGRAM_BOT_USERNAME=mi_bot
TELEGRAM_API_BASE=https://api.telegram.org
TELEGRAM_POLL_TIMEOUT=20
TELEGRAM_NOTIFY_ALWAYS=0
TELEGRAM_LINK_TTL_MIN=15
TELEGRAM_CALL_URI=cws
EXTERNAL_API_SECRET=<secreto-largo-aleatorio>

# Llamadas / ICE-TURN
STUN_URLS=stun:stun.sodastream.fun:3478
TURN_URLS=turn:turn.sodastream.fun:3478?transport=udp,turns:turn.sodastream.fun:5349?transport=tcp
TURN_SECRET=<secreto-de-coturn>
TURN_TTL_SECONDS=3600

# E2EE / identidad
CALL_E2EE_REQUIRED=true
CALL_ID_SERVER=true
```
