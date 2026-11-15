# Chat 1:1 — contrato de integración (cliente)

Guía para implementar el chat uno a uno desde el cliente (Android) contra
`https://cws.sodastream.fun`. El cliente **solo usa identificadores públicos**;
`usu_id` es interno y nunca se envía ni se espera.

## Estado

| Parte | Estado |
|---|---|
| Login Bearer, `POST /messaging/setup`, `GET /messaging/users/:id` | ✅ implementado |
| WebSocket `/ws/v1` con auth Bearer | ✅ implementado |
| Historial (`GET /conversations/.../messages`) | ✅ implementado en código |
| Envío (`POST /conversations/.../messages`) + push `dm` | ✅ implementado en código |
| Marcar leído (`POST /conversations/.../read`) | ✅ implementado en código |
| Tabla `dbo.opr_chat_mensajes` | ✅ aplicada |
| SP `procChatMensajesCons` / `procChatMensajesProc` | 📄 aplicar desde `db_sodastream/` |

Los endpoints de chat devuelven `500` hasta que se apliquen los dos SP en la
base de datos. El contrato (paths, JSON, códigos) no cambia.

## Identificadores

| Uso | Campo | Formato |
|---|---|---|
| Usuario en el cliente | `usu_id_public` | nanoid 12 (`A-Za-z0-9_-`) |
| Usuario interno | `usu_id` | **NO se usa** (solo server-side) |
| Mensaje | `msg_id` | entero, cursor de paginación |

- `usu_id_public` **propio**: lo devuelve `POST /messaging/setup`. Guárdalo.
- `usu_id_public` **del contacto**: viene de tu lista de usuarios o de
  `GET /messaging/users/:public_id`.
- Nunca compares ni muestres `usu_id`.

## Flujo

```
1. Login                 → access_token (Bearer)
2. Setup (una vez)       → mi usu_id_public
3. Conectar WS (Bearer)  → recibe evento `dm`
4. Click en contacto     → contacto.usu_id_public
5. GET historial         → mensajes (msg_id, from, es_mio, texto, ...)
6. Render                → burbuja propia si msg.from == mi usu_id_public
7. Enviar (HTTP POST)    → 201 con msg_id; el receptor recibe `dm` por WS
8. Paginar               → before=<msg_id más viejo cargado>
9. Responder             → reply_to=<msg_id citado>
```

## Endpoints (Bearer)

Todas las llamadas llevan `Authorization: Bearer <access_token>`. Si el
access caduca: refresh con `X-Refresh-Token` y **un** reintento (ver
`WS_README.md` → *Reconexión y refresh*).

### 1. Habilitar mensajería (una vez)

```http
POST /api/v1/messaging/setup
```

**201**
```json
{"success":true,"error":0,"msg":"Mensajeria instantanea habilitada",
 "data":{"usu_id_public":"VN2qxVIsbz5t","ws_path":"/ws/v1"}}
```

- Guarda `usu_id_public`. **Cada llamada regenera el id**; no la repitas en
  cada arranque. Si rotas el id, los contactos con el id anterior dejan de
  poder resolverte.

### 2. Buscar contacto por id público

```http
GET /api/v1/messaging/users/{usu_id_public}
```

**200**
```json
{"success":true,"error":0,"msg":"Usuario encontrado",
 "data":{"usu_id_public":"VN2qxVIsbz5t","usu_nombre":"KDF2"}}
```

- `400` formato inválido · `404` no existe · `401` sin token.

### 3. Historial de la conversación

```http
GET /api/v1/messaging/conversations/{usu_id_public}/messages?before={msg_id}&limit=30
```

- `:usu_id_public` = contacto. El servidor resuelve tu `usu_id` desde el token.
- `before` opcional: devuelve mensajes con `msg_id < before`.
- `limit` por defecto 30, máximo 100.
- Orden: **más nuevo primero**.

**200**
```json
{"success":true,"error":0,"msg":"Mensajes obtenidos",
 "data":{
   "messages":[
     {"msg_id":124,"from":"AB12cd34EF56","es_mio":false,
      "texto":"hola","fecha":"2026-11-13T18:05:00",
      "reply_to":null,"leido":false},
     {"msg_id":123,"from":"VN2qxVIsbz5t","es_mio":true,
      "texto":"qué tal","fecha":"2026-11-13T18:04:00",
      "reply_to":null,"leido":true}
   ],
   "next_before":123
 }
}
```

- `next_before`: `msg_id` del mensaje más viejo de la página si puede haber
  más; `null` cuando no hay más.
- `from` es el `usu_id_public` del emisor. `es_mio` lo calcula el servidor
  (conveniencia; equivale a `from == mi usu_id_public`).
- `reply_to` es un `msg_id` o `null`.

### 4. Enviar mensaje

```http
POST /api/v1/messaging/conversations/{usu_id_public}/messages
Content-Type: application/json

{"texto":"hola","reply_to":123}
```

- `texto`: obligatorio, no vacío. Límite 4096 bytes UTF-8 (~4000 chars).
- `reply_to`: opcional; debe ser un `msg_id` de la misma conversación.

**201**
```json
{"success":true,"error":0,"msg":"Mensaje enviado",
 "data":{"msg_id":125,"from":"VN2qxVIsbz5t","es_mio":true,
         "texto":"hola","fecha":"2026-11-13T18:06:00",
         "reply_to":123,"leido":false}}
```

- `400` texto/reply inválido · `404` contacto no existe · `401` sin token.
- Tras el 201, el servidor emite `dm` por WS al receptor y a tus otros
  dispositivos. Deduplica por `msg_id`: el que envió ya tiene el mensaje del
  201.

### 5. Marcar leído (opcional)

```http
POST /api/v1/messaging/conversations/{usu_id_public}/read
Content-Type: application/json

{"hasta":124}
```

Marca como leídos los mensajes del contacto con `msg_id <= hasta`.

## WebSocket (recepción en tiempo real)

- URL: `/ws/v1`; upgrade con `Authorization: Bearer <access_token>`.
- El handshake usa **GOST/Streebog-256** (no SHA-1): usa el mismo cliente o
  puente que ya funciona (ver `WS_README.md`). Un cliente WS estándar
  (OkHttp puro) **no** completa el handshake.
- Envelope: `evento '\n' payload`.

### Evento `dm`

```
dm
{"msg_id":126,"from":"AB12cd34EF56","from_nombre":"KDF2",
 "texto":"respuesta","fecha":"2026-11-13T18:07:00","reply_to":125}
```

- Llega al receptor y a tus otros dispositivos (si enviaste desde uno).
- Deduplica por `msg_id` (el emisor ya tiene el suyo del POST).
- Ignora `welcome.identity.public_id` para el chat: es el id de la conexión
  (`socket_id`); tu identidad de chat es `usu_id_public` (del setup).

## Reglas para el cliente

1. **No uses `usu_id`.** Ni lo pidas, ni lo guardes, ni lo compares.
2. **Identidad propia** = `usu_id_public` del setup; **contacto** =
   `usu_id_public` de la lista/lookup.
3. **Compara `from`** con tu `usu_id_public` (o usa `es_mio`) para decidir la
   burbuja. No compares `msg_id` entre usuarios.
4. **Deduplica por `msg_id`** (POST 201 + `dm` pueden repetir el mismo).
5. **Paginación**: `next_before` → `before`; no recargues todo.
6. **Reintentos**: si el POST falla por timeout, **no** lo repitas a ciegas
   (el servidor aún no deduplica envíos); refresca el historial y verifica.
7. **No llames a `setup` en cada arranque** (regenera el id). Guárdalo.
8. **Refresh/reconexión**: refrescar el token no afecta al WS abierto; al
   reconectar usa el access más reciente (ver `WS_README.md`).
9. **Logout**: cierra el WS y borra el `usu_id_public` local.
10. **Contactos**: la agenda sale de tu lista de usuarios; para este flujo no
    se necesita tabla de contactos en el servidor.

## Android — checklist

- [ ] Guardar `access_token` y `usu_id_public` (DataStore).
- [ ] `setup` una sola vez (si falta el id).
- [ ] Conectar WS con Bearer y el cliente GOST; manejar `dm`.
- [ ] Historial al abrir el chat (`limit=30`, luego paginar).
- [ ] Render con `msg.from == mi usu_id_public` (o `es_mio`).
- [ ] Enviar con POST; UI optimista con id temporal; reemplazar por `msg_id`.
- [ ] Dedupe por `msg_id` al recibir `dm`.
- [ ] Reconnect con backoff y refresh single-flight.
- [ ] `reply_to` al citar; mostrar cita con el `msg_id`.
- [ ] `read` al abrir/visible (opcional).

## Notas

- `usu_id_public` es público: no es un secreto; sirve para direccionar el
  chat. No lo uses como autenticación.
- El servidor guarda `emisor`/`receptor` como `usu_id` internos y los traduce
  a ids públicos en las respuestas.
- La tabla no tiene FK por decisión de diseño; la validación de `reply_to` y
  del texto es responsabilidad del servidor.

## Referencias

- `docs/endpoints.md` — endpoints HTTP.
- `WS_README.md` — conexión WS, auth, reconexión y límites.
- `AUTH_README.md` — login/refresh Bearer.
- `db_sodastream/propuesta_chat_mensajes.sql` — tabla de mensajes.
