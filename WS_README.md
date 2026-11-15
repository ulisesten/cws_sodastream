# WebSocket API v1 (`/ws/v1`)

Endpoint WebSocket nativo de `cws_sodastream` sobre la lib `cws`. Usa framing
**RFC 6455** pero el handshake **NO usa SHA-1**: usa **GOST Streebog-256**
(GOST R 34.11-2012). Esto implica que **un navegador estándar no puede
conectar**; hace falta un cliente con digest GOST o un puente de
compatibilidad (ver [Compatibilidad con clientes](#compatibilidad-con-clientes-gost)).

- **Ruta**: `/ws/v1` (path exacto; sin subprotocolos)
- **Producción**: `wss://cws.sodastream.fun/ws/v1` (si el servidor/reverso tiene TLS)
- **Local**: `ws://127.0.0.1:8080/ws/v1`
- **Código servidor**: `src/ws_api/v1/ws_v1.{h,c}` (montado desde `src/main.c`)
- **Hub de la lib**: `third_party/cws` (`include/cws/websocket.h`)

> **Autenticación obligatoria.** No hay invitados. El upgrade exige
> `Authorization: Bearer <access_token>` (canal móvil/Bearer). Sin token
> válido → `401` y no se completa el WebSocket. El token se valida **solo en
> el upgrade** (ver [Reconexión y refresh](#reconexión-y-refresh-del-access-token)).

---

## Compatibilidad con clientes (GOST)

La lib calcula el `Sec-WebSocket-Accept` así:

```
accept = base64( Streebog256( Sec-WebSocket-Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" ) )
```

En vez del SHA-1 que exige RFC 6455. Como el **cliente** valida ese valor
contra el digest del protocolo (SHA-1), cualquier stack que cumpla el RFC al
pie de la letra **rechazará** la conexión:

| Cliente | ¿Conecta? | Motivo |
|---|---|---|
| Navegador (`new WebSocket(...)`) | **No** | valida el accept con SHA-1; no se puede sustituir desde JS |
| `websocat`, `wscat`, `ws` (Node) | **No** | validan SHA-1 |
| Cliente propio (Python/Node/C/Rust) con Streebog | **Sí** | calcula el accept GOST |
| Puente WS↔WS (ver abajo) | **Sí** | termina SHA-1 del lado navegador y GOST del lado servidor |

### ¿Hay que desarrollar un módulo de compatibilidad?

**Sí**, para el front web. El navegador no permite cambiar el digest del
handshake. Opciones:

1. **Puente / proxy WebSocket (recomendado para el front web).** Servicio que
   acepta WebSocket **estándar (SHA-1)** del navegador y abre WebSocket
   **GOST** hacia `cws`, reenviando frames. El puente debe además autenticarse
   contra `cws` con `Authorization: Bearer <access_token>` (el navegador no
   puede poner esa cabecera en `new WebSocket()`; el puente la añade).
2. **Cliente GOST dentro del front** si corre en Node/Electron (no navegador):
   handshake con Streebog + framing RFC 6455 + cabecera Bearer.
3. **Runtime parcheado / extensión** (no recomendado).

> Tokens y contraseñas también son GOST, pero el único punto GOST **del canal
> WS** es el digest del handshake. El payload viaja en claro dentro del frame
> (usar `wss`/TLS).

---

## Conexión

### 1. Obtener el access token

El WS usa el mismo token Bearer del canal móvil (ver `AUTH_README.md`):

```http
POST /api/v1/auth/mobile/signin
Content-Type: application/json

{"usu_correo":"user@test.local","usu_contrasena":"..."}
```

→ `access_token` en el body.

### 2. Upgrade con auth

```http
GET /ws/v1 HTTP/1.1
Host: 127.0.0.1:8080
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Version: 13
Sec-WebSocket-Key: <16 bytes aleatorios en base64>
Authorization: Bearer <access_token>
```

El servidor valida firma + expiración + `session_type` móvil y **revalida el
`salt` contra el DAO** (logout/revocación) antes del `101`. Respuestas posibles:

| Código | Caso |
|---|---|
| `101` | aceptado |
| `401` | sin `Authorization: Bearer` (`No credentials are present.`) |
| `401` | token inválido/expirado/revocado (`Authentication rejected.`) |
| `426` | faltan `Upgrade`/`Connection` o versión ≠ 13 |
| `400` | `Sec-WebSocket-Key` ausente o > 80 bytes |
| `503` | máximo de conexiones |

`Sec-WebSocket-Accept`:

```http
Upgrade: websocket
Connection: Upgrade
Sec-WebSocket-Accept: <base64(Streebog256(key + GUID))>
```

---

## Reconexión y refresh del access token

**Cuándo se valida el token**: solo en el **upgrade** (handshake). Una vez
establecido el WebSocket, el servidor **no** vuelve a comprobar el access token
por cada mensaje ni cuando éste expira.

| Situación | Efecto sobre el WS ya abierto | Qué debe hacer el cliente |
|---|---|---|
| `POST /api/v1/auth/mobile/refresh_token` (nuevo access) | **Ninguno**: el socket mantiene la identidad del handshake | No reconectar por haber refrescado; guardar el nuevo access |
| El access caduca con el WS abierto | **Ninguno**: el socket sigue vivo | Seguir usando el WS |
| Reconexión (pérdida de red, resume, etc.) | El nuevo upgrade valida el token que se envíe | Usar el access **más reciente** |
| Reconectar con el access viejo/expirado | `401` **antes** del `101` | Refresh (single-flight) + reintento **único** |
| Logout (rota `usu_salt`) | El socket abierto **no** se cierra solo | Cerrar el WS localmente al hacer logout |
| Reinicio del servidor | El socket se cae | Reconectar con backoff |

### Reglas de reconexión

1. **No reconectes solo porque refrescaste.** El refresh no afecta al socket
   abierto; reconecta solo por fallo de transporte, cambio de red o resume.
2. **Al reconectar, usa el access token vigente**, leído de almacenamiento
   seguro en ese momento. No una copia capturada al construir el cliente WS.
3. **No uses el refresh token como Bearer del WS**: el upgrade valida un
   *access* token (`kind=access`); un refresh se rechaza con `401`.
4. **Single-flight del refresh**: si hay un refresh en curso y toca
   reconectar, espera su resultado y usa el token nuevo (evita carreras).
5. **`401` en el upgrade → refresh + un único reintento.** Si vuelve a `401`,
   forzar login. No reintentar en bucle.
6. **Backoff exponencial con jitter** (p. ej. 1s, 2s, 4s… tope ~30s) y resetear
   al conectar.
7. **Cancela el loop de reconexión en logout** y no lo reinicies con un token
   revocado.
8. El `401` del upgrade es una respuesta HTTP **antes** del `101`; en OkHttp se
   observa en el `Response.code == 401` del handshake, no como frame WS.
9. Si en el futuro el servidor cierra por token expirado (código de aplicación
   `4001`), haz refresh y reconecta.

### Pitfalls típicos en Android

- Guardar el access token al crear el cliente WS y reutilizarlo tras refrescar.
- Un loop de reconexión (coroutine/Flow) que capturó un token viejo.
- Reconectar **antes** de que termine el refresh (usa el token viejo).
- Tratar el `401` del upgrade como error fatal y no refrescar.
- Refrescar y reconectar en **cada** refresh (churn innecesario).
- Varios reconectadores en paralelo con tokens distintos.
- No cerrar el WS al hacer logout.

### Pseudocódigo (Kotlin/OkHttp)

```kotlin
suspend fun connectWs() {
    while (active) {
        val token = tokenStore.accessToken()      // el más reciente
        val resp  = wsUpgrade(token)              // Authorization: Bearer <token>
        if (resp.code == 101) {
            backoff.reset()
            runSocket(resp)                       // hasta que se cierre
            continue                              // caída -> reintentar
        }
        if (resp.code == 401) {
            if (!refreshSingleFlight()) { logout(); return }
            continue                              // un reintento con el token nuevo
        }
        delay(backoff.next())                     // red/otros errores
    }
}
```

> **Mecanismo del servidor (transparente):** el upgrade valida token + salt
> una vez y guarda la identidad en el socket. **No hay revalidación periódica**
> ni cierre automático por expiración o refresh. El logout invalida tokens
> futuros, pero **no** cierra sockets ya abiertos (pendiente).

---

## Framing (RFC 6455)

- Opcodes: `0x1` texto, `0x2` binario, `0x8` close, `0x9` ping, `0xA` pong.
- **Cliente → servidor: obligatorio enmascarar**.
- Servidor → cliente: frames **sin máscara**.
- Fragmentación, control frames y close handshake estándar.
- **Sin compresión** (`permessage-deflate` no soportado).

---

## Identidad

Al conectar, el servidor publica:

```json
{"public_id":"ebfff891c3979b6901a25075","nombre":"KDF2"}
```

- `public_id`: **id público de la conexión** (`socket_id`, 24 hex). Cambia al
  reconectar.
- `nombre`: `usu_nombre` del token autenticado (autoritativo).
- **`usu_id` es interno y nunca se emite** (ni en `welcome`, ni en `identity`,
  ni en mensajes). Solo se usa server-side (p. ej. sala `user:<usu_id>`).

**Id público persistente (chat 1:1):** `cat_usuarios.usu_id_public`, generado
con `POST /api/v1/messaging/setup` (Bearer, ver `docs/endpoints.md`). Es
nanoid de 12 y no cambia entre conexiones; hoy los mensajes de sala todavía
llevan el `public_id` de la conexión (`socket_id`).

La identidad va en `welcome` y se puede pedir con `whoami`.

---

## Protocolo de aplicación

El payload de texto se interpreta como `evento '\n' payload`. Si no hay `\n`,
todo el mensaje es el nombre del evento.

### Cliente → servidor

| Mensaje | Respuesta | Descripción |
|---|---|---|
| `ping\n<dato>` | `pong\n<dato>` | latido de aplicación |
| `echo\n<dato>` | `echo\n<dato>` | eco |
| `join\n<room>` | `joined\n<room>` | une el socket a una sala |
| `leave\n<room>` | `left\n<room>` | saca el socket de una sala |
| `message\n<texto>` | `message\n<json>` a la sala `lobby` | envía mensaje |
| `message\n<room>\n<texto>` | `message\n<json>` a `<room>` | envía mensaje a una sala |
| `whoami\n` | `identity\n<json>` | identidad pública del socket |
| `stats\n` | `stats\n<json>` | métricas del hub |
| *(otro)* | `error\nevento desconocido` | fallback |

### Servidor → cliente

| Evento | Payload | Cuándo |
|---|---|---|
| `welcome` | `{"socket_id","path","ts","identity":{...}}` | al abrir |
| `pong` | eco del `ping` | respuesta a `ping` |
| `echo` | mismo dato | respuesta a `echo` |
| `joined` | nombre de sala | tras `join` |
| `left` | nombre de sala | tras `leave` |
| `message` | `{"public_id","usu_nombre","msg","room","ts"}` | mensaje de la sala |
| `dm` | `{"msg_id","from","from_nombre","texto","fecha","reply_to"}` | mensaje 1:1 (push del POST de chat) |
| `identity` | `{"public_id","nombre"}` | respuesta a `whoami` |
| `stats` | JSON de `cws_ws_stats_t` | respuesta a `stats` |
| `error` | texto | formato inválido / evento desconocido |

Al abrir, el socket se une automáticamente a **`lobby`** y a la sala privada
`user:<usu_id>` (interna).

El chat 1:1 llega como evento **`dm`** (servidor → cliente) a esa sala; el
envío se hace por HTTP (`POST /api/v1/messaging/conversations/:public_id/messages`).
Contrato completo en `docs/chat_1a1_cliente.md`.

---

## Mensajes (chat)

### Enviar

```text
cliente -> message\nhola mundo
```

o a una sala concreta (hay que estar unido a ella con `join`):

```text
cliente -> join\nvideo:42
servidor -> joined\nvideo:42
cliente -> message\nvideo:42\nhola a la sala
```

### Recibir (canónico)

El servidor **compone** el mensaje (no se confía en el nombre que mande el
cliente) y lo emite a **todos** los miembros de la sala, incluido el emisor:

```json
{
  "public_id": "ebfff891c3979b6901a25075",
  "usu_nombre": "KDF2",
  "msg": "hola a la sala",
  "room": "video:42",
  "ts": 1792702888
}
```

Reglas:

- `public_id` = `socket_id` (id público de la conexión).
- `usu_nombre` = nombre del usuario **autenticado** (del token), no del payload.
- `usu_id` **no** aparece.
- Texto máximo: **4096 bytes** (se responde `error` si se supera o está vacío).
- Para salas: únete antes con `join`; si no eres miembro, el mensaje no te
  llegará (aunque sí a los miembros de la sala).
- Sin historial: lo emitido sin miembros conectados se pierde.

---

## Salas (rooms)

- Hasta **64** salas por socket.
- `join`/`leave` responden solo al emisor (`joined`/`left`).
- Salas usadas por el servidor: `lobby` (auto-join) y `user:<usu_id>`
  (auto-join, privada; el `usu_id` no se expone).
- Convención recomendada: `video:<vid_id>` (pública), `user:<usu_id>`
  (privada), `admin` (privada).
- **Autorización de salas privadas: pendiente** (hoy `join` no valida
  permisos; ver `WS_README` seguridad).

---

## Límites, heartbeat y configuración

| Variable | Default | Descripción |
|---|---|---|
| `WS_MAX_FRAME_SIZE` | `1048576` (1 MiB) | tamaño máximo de frame |
| `WS_MAX_MESSAGE_SIZE` | `4194304` (4 MiB) | mensaje reensamblado |
| `WS_MAX_BUFFERED_BYTES` | `4194304` (4 MiB) | buffer de salida por socket (close `1009` si se excede) |
| `WS_PING_INTERVAL_MS` | `30000` | ping del servidor; `0` desactiva |
| `WS_PING_TIMEOUT_MS` | `10000` | timeout para el pong |
| `WS_MAX_CONNECTIONS` | `65536` | conexiones simultáneas |

El servidor envía **ping** cada 30 s; el cliente debe responder **pong** o se
cierra a los 10 s.

---

## Cliente de referencia (Python, probado)

Dependencias: [`gostcrypto`](https://pypi.org/project/gostcrypto/) (Streebog-256).

```python
import socket, base64, os, struct, json, urllib.request
from gostcrypto import gosthash

BASE, HOST, PORT, PATH = "http://127.0.0.1:8080", "127.0.0.1", 8080, "/ws/v1"
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

# 1) access token (canal móvil/Bearer)
req = urllib.request.Request(
    BASE + "/api/v1/auth/mobile/signin",
    data=json.dumps({"usu_correo": "user@test.local",
                     "usu_contrasena": "..."}).encode(),
    headers={"Content-Type": "application/json"})
access = json.load(urllib.request.urlopen(req))["access_token"]

# 2) upgrade GOST + Bearer
key = base64.b64encode(os.urandom(16)).decode()
s = socket.create_connection((HOST, PORT), 5)
s.sendall((f"GET {PATH} HTTP/1.1\r\nHost: {HOST}:{PORT}\r\n"
           "Upgrade: websocket\r\nConnection: Upgrade\r\n"
           f"Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: {key}\r\n"
           f"Authorization: Bearer {access}\r\n\r\n").encode())

buf = bytearray()
while b"\r\n\r\n" not in buf:
    buf += s.recv(4096)
head, rest = bytes(buf).split(b"\r\n\r\n", 1)
assert b" 101 " in head.split(b"\r\n")[0], head

class WS:
    def __init__(self, sock, extra):
        self.s, self.b = sock, bytearray(extra)
    def _read(self, n):
        while len(self.b) < n:
            d = self.s.recv(4096)
            if not d: raise EOFError
            self.b += d
        out = bytes(self.b[:n]); del self.b[:n]; return out
    def frame(self):
        b0, b1 = self._read(2)
        op, masked, n = b0 & 0x0F, b1 >> 7, b1 & 0x7F
        if n == 126: n = struct.unpack(">H", self._read(2))[0]
        elif n == 127: n = struct.unpack(">Q", self._read(8))[0]
        mk = self._read(4) if masked else None
        d = self._read(n)
        if mk: d = bytes(x ^ mk[i % 4] for i, x in enumerate(d))
        return d.decode(errors="replace")
    def send(self, text):
        d = text.encode(); mk = os.urandom(4); n = len(d)
        h = bytearray([0x81])
        if n < 126: h.append(0x80 | n)
        else: h.append(0x80 | 126); h += struct.pack(">H", n)
        h += mk + bytes(x ^ mk[i % 4] for i, x in enumerate(d))
        self.s.sendall(bytes(h))

ws = WS(s, rest)
print(ws.frame())                              # welcome + identity
ws.send("whoami");         print(ws.frame())   # identity
ws.send("message\nhola");  print(ws.frame())   # message {public_id, usu_nombre, ...}
ws.send("join\nvideo:42"); print(ws.frame())   # joined
ws.send("message\nvideo:42\nhola sala"); print(ws.frame())  # message
s.close()
```

Salida esperada (payloads):

```
welcome ... {"identity":{"public_id":"<socket_id>","nombre":"..."}}
identity {"public_id":"<socket_id>","nombre":"..."}
message {"public_id":"<socket_id>","usu_nombre":"...","msg":"hola","room":"lobby","ts":...}
joined video:42
message {"public_id":"<socket_id>","usu_nombre":"...","msg":"hola sala","room":"video:42","ts":...}
```

### Node.js

El paquete `ws` **no sirve** como cliente (valida SHA-1). Hay que implementar
el handshake con Streebog (p. ej. `gost-crypto`) sobre `net`/`tls`, añadir la
cabecera `Authorization: Bearer` y enmascarar los frames salientes.

---

## Seguridad / pendientes

- **Auth obligatoria** (implementada): Bearer + exp + salt contra el DAO en el
  upgrade. Sin token válido no hay conexión.
- El token se valida al conectar; si caduca durante la sesión, la conexión
  sigue. Pendiente: revalidar/cerrar con un código de aplicación (p. ej.
  `4001`) y reconectar con token refrescado.
- **Salas privadas** (`user:<id>`, `admin`): `join` aún no valida permisos.
- `public_id` es el id de la conexión (display); `usu_nombre` es autoritativo.
- Validar `Origin` para evitar **CSWSH** (el middleware CORS global ya expone
  `CORS_ORIGINS`; falta aplicarlo al upgrade).
- Rate limit de mensajes por socket.
- Canal sin cifrar: usar `wss`/TLS.

## Verificación rápida

```bash
# Sin auth -> 401
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8080/ws/v1

# Con auth + upgrade -> 101 (GOST; curl no completa el handshake por SHA-1)
# Usar el cliente Python de arriba.
```
