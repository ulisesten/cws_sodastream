# feat(websocket): WebSocket nativo (RFC 6455) con API estilo Socket.IO

## Objetivo

Añadir a la librería `cws` un módulo **WebSocket nativo (RFC 6455)** con una API
en C **inspirada en Socket.IO** (`on`, `emit`, `join`, `to`, IDs fáciles), pero
**desacoplada** del resto de la librería (HTTP, auth, SQL, JSON) y preparada
para **escalar** a varios workers/threads y, a futuro, a varios procesos.

Se busca el *estilo* de Socket.IO, **no** su protocolo completo: transporte WS
estándar, eventos con nombre y rooms.

## Convenciones de la librería a respetar

- Organización **por feature**: `include/cws/websocket.h` + `src/websocket/*.c`
  (p. ej. `websocket.c`, `frame.c`, `rooms.c`, `adapter.c`); incluirlo en
  `include/cws/cws.h`.
- Añadir los `.c` a `CWS_SRC` en `CMakeLists.txt`.
- C11, sin excepciones de estilo: nombres `cws_*` / tipos `cws_*_t`, macros
  `CWS_*`.
- Toda función fallible devuelve `int` con `CWS_OK` / `CWS_ERR_*`
  (`include/cws/errors.h`).
- Comentarios Doxygen (`@brief`, `@param`, `@return`) como el resto de la lib.
- Sin dependencias externas **obligatorias**. SHA1 y Base64 se implementan en
  `src/utils/` (no existen hoy).
- Respetar el modelo sin allocations en el hot path: buffers por conexión.

## Alcance

- Protocolo de cable: **RFC 6455 puro** → cualquier cliente WS estándar
  (navegador, `websocat`, `wscat`, OkHttp en Android) conecta sin shims.
- API C con semántica Socket.IO: eventos con nombre, rooms, `join`/`to`, IDs.
- Integración con el `epoll`/workers/timers ya existentes en `cws`.

## Fuera de alcance (por ahora)

- **Engine.IO**: handshake propio, long-polling, upgrades, fallback.
- Compatibilidad con `socket.io-client` (namespaces, acks, binario adjunto,
  `socket.io-parser`).
- Parsing de JSON/payloads: la lib transporta **bytes opacos**; el JSON lo
  decide el consumidor.
- Auth/sesiones/persistencia: vía **hook** (ver RF-1).
- TLS: lo aporta `cws`; `wss://` = WS sobre la conexión TLS ya descifrada.

## Principios de diseño (desacoplamiento)

1. La lib **no** conoce auth, SQL, GOST ni usuarios. Solo expone un **hook de
   upgrade** donde la app decide aceptar o rechazar.
2. La lib **no** crea hilos ni event loops propios: usa los de `cws`.
3. La app **no** toca frames ni sockets: solo registra/emite eventos y rooms.
4. **Todo** el fan-out pasa por una abstracción **adapter** (in-process por
   defecto; pub/sub opcional) para soportar N workers y N procesos sin cambiar
   el código de la app.
5. Compatible en *espíritu* con Socket.IO, pero **no acoplada**: el transporte
   es WS estándar y el diseño deja abierta una futura capa de protocolo.

## API propuesta (`cws/websocket.h`)

```c
/* Ciclo de vida */
int cws_ws_on_open   (cws_ws_t* ws, cws_ws_open_fn cb, void* ud);
int cws_ws_on_close  (cws_ws_t* ws, cws_ws_close_fn cb, void* ud);
int cws_ws_on_error  (cws_ws_t* ws, cws_ws_error_fn cb, void* ud);

/* Eventos (global o por socket) */
int cws_ws_on(cws_ws_t* ws, const char* event, cws_ws_event_fn cb, void* ud);
int cws_ws_socket_on(cws_ws_socket_t* s, const char* event,
                     cws_ws_event_fn cb, void* ud);

/* Emitir */
int cws_ws_emit(cws_ws_socket_t* s, const char* event,
                const void* data, size_t len);
int cws_ws_emit_all(cws_ws_t* ws, const char* event,
                    const void* data, size_t len);

/* Rooms */
int    cws_ws_join(cws_ws_socket_t* s, const char* room);
int    cws_ws_leave(cws_ws_socket_t* s, const char* room);
size_t cws_ws_socket_rooms(cws_ws_socket_t* s, const char* const** out);

/* Emisor dirigido: io.to(room).emit / socket.to(room).emit */
int cws_ws_to(cws_ws_t* ws, const char* room,
              const char* event, const void* data, size_t len);
int cws_ws_socket_to(cws_ws_socket_t* s, const char* room,
                     const char* event, const void* data, size_t len);

/* Consultas */
cws_ws_socket_t* cws_ws_socket_by_id(cws_ws_t* ws, const char* id);
size_t           cws_ws_count(cws_ws_t* ws);
size_t           cws_ws_room_size(cws_ws_t* ws, const char* room);
```

- El id del socket es único, legible y estable (`cws_ws_socket_id(s)`), 16–24
  chars; la app puede asociar su propio contexto (`cws_ws_socket_data(s)`).
- `emit` **no bloqueante**: encola en el buffer de salida del socket.

### Semántica estilo Socket.IO

- `socket.to(room).emit` → a la room, **excluyendo al emisor**.
- `io.to(room).emit` / `cws_ws_to` → a **todos** los de la room.
- `cws_ws_emit_all` → a todas las conexiones.
- `join`/`leave` idempotentes; salas dinámicas creadas/destruidas al quedar
  vacías.
- El broadcast debe iterar sobre una copia segura del set para permitir
  `leave`/`close` durante el callback.

## Framing (RFC 6455)

- Opcodes: continuation `0x0`, text `0x1`, binary `0x2`, close `0x8`,
  ping `0x9`, pong `0xA`.
- Fragmentación: reassembly con `FIN=0` + continuation.
- Máscara **obligatoria** cliente→servidor (cerrar `1002` si falta); el server
  no enmascara.
- Longitudes 7/16/64 bits (`126`/`127`).
- Control frames ≤125 B, no fragmentables; `ping` → `pong` automático.
- Close handshake con echo del código y timeout.
- Límites `max_frame_size`/`max_message_size` → `1009`; UTF-8 inválido en
  `text` → `1007`.

## Handshake y hook de autorización (RF-1)

- Detectar `Upgrade: websocket` + `Connection: Upgrade` en una petición HTTP.
- `Sec-WebSocket-Version: 13` (si no, `426`).
- Responder `101` con `Upgrade`, `Connection: Upgrade` y
  `Sec-WebSocket-Accept = base64(SHA1(key + GUID))`.
- **Hook `on_upgrade(socket, req, res)`** de la app, ejecutado **antes** del
  `101`; si rechaza, se responde HTTP `401/403` normal y se cierra. Debe
  exponer headers/cookies/query de `cws_request_t` para validar el token.
- Registrar el upgrade dentro del ciclo HTTP:
  `cws_app_ws(app, "/ws", on_upgrade, cb, ud)` (o middleware de path).
- Tras el `101`, la conexión pasa de estado "HTTP" a "WS" en el mismo
  `cws_conn`/`epoll`; **no** cerrar por keep-alive.
- Apagado ordenado: enviar `close` a todos y liberar rooms/IDs.

## Escalabilidad (adapter)

```c
typedef struct cws_ws_adapter {
    int (*publish)  (void* ctx, const char* room,
                     const void* frame, size_t len);
    int (*subscribe)(void* ctx, const char* room, cws_ws_adapter_cb cb);
    int (*join)     (void* ctx, const char* room, const char* socket_id);
    int (*leave)    (void* ctx, const char* room, const char* socket_id);
    int (*members)  (void* ctx, const char* room, /* out */ ...); /* opcional */
} cws_ws_adapter_t;
```

- Por defecto **`inproc`**.
- `n_workers == 1`: fan-out directo.
- N workers con `SO_REUSEPORT`: un cliente cae en un worker concreto; el
  fan-out cross-worker lo garantiza la lib (registro global de rooms + mutex,
  o cola central vía `eventfd`/pipe, o el adapter).
- Activar Redis/NATS **no** debe cambiar el código de la app: solo config.

## Configuración

En `cws_config_t` / env:

- `ws_max_frame_size`, `ws_max_message_size`, `ws_max_buffered_bytes`.
- `ws_ping_interval_ms`, `ws_ping_timeout_ms` (heartbeat; cerrar sin pong).
- `ws_max_rooms_per_socket`, `ws_max_connections`.
- `ws_adapter` (`inproc` | `redis` | `nats`), `ws_adapter_url`.

## Métricas

`ws_connections_active`, `ws_connections_total`, `ws_messages_in/out`,
`ws_bytes_in/out`, `ws_closes_by_code`.

## Criterios de aceptación

- **Interop**: navegador (`WebSocket`), `websocat`, `wscat`.
- **Handshake**: vector RFC 6455 — key `dGhlIHNhbXBsZSBub25jZQ==` →
  accept `s3pPLMBiTxaQ9kYGzzhZRbK+xOo=`.
- Frames 7/16/64 bits, fragmentados, `ping`/`pong`, `close` con código.
- Rechazo de frames sin máscara.
- `wss://` a través del TLS de `cws`.
- Broadcast por room con N workers (adapter `inproc`).
- Heartbeat cierra conexiones zombie.
- Backpressure: `on_drain` al vaciar y política al exceder el tope.
- Benchmarks de throughput y conexiones concurrentes.

## Fases sugeridas

1. Handshake + framing + callbacks (WS puro, 1 worker).
2. Rooms + `to`/`emit` + IDs.
3. Integración multi-worker + adapter `inproc`.
4. Métricas, config, backpressure y heartbeat.
5. (Opcional) adapter Redis/NATS.
6. (Futuro, solo si se pide) capa Engine.IO/Socket.IO. **No implementar ahora.**

## Notas del consumidor (cws_sodastream)

- La app implementará `on_upgrade` reusando su validación existente (cookie
  `access_token` web / `Bearer` móvil) y **no** dejará que la lib conozca auth.
- Rooms previstas por entidad, p. ej. `user:<usu_id>`, `room:<id>`,
  `video:<id>`; payloads JSON a cargo de la app.
- Casos de uso: notificaciones/progreso de transcodificación, eventos de
  catálogo, presencia.
- El front (`streaming_server`) usa WS estándar; no requiere `socket.io-client`.
