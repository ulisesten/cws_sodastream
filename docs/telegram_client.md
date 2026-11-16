# Cliente: integración de llamadas + Telegram

Guía para Android nativo (Kotlin+JNI), Linux y Windows (Qt+C).
Estado actual: **puente de notificación/aceptación**; el media WebRTC
(SDP/ICE/DTLS-SRTP, E2EE GOST) es la fase siguiente.

## 1. Resumen

- La app mantiene un **WebSocket `/ws/v1`** autenticado con Bearer.
- Si el receptor tiene WSS vivo, el aviso llega por `call.incoming`; **no hace
  falta Telegram ni FCM**.
- Si no hay WSS, el servidor manda un mensaje de Telegram con
  `cws://call/<call_id>` (si el usuario vinculó su cuenta).
- El receptor acepta con `POST /accept` y aporta su IP/teléfono (opcionales).
- El solicitante recibe `call.accepted` con la IP del receptor.
- `call_id` es la clave de la llamada; `usu_id` (interno) une usuario,
  teléfono/IP y chat de Telegram.

## 2. Datos que maneja el cliente

| Dato | Obligatorio | Notas |
|---|---|---|
| Access token Bearer | Sí | 5 min; refresh con `X-Refresh-Token` |
| `usu_id_public` del receptor | Sí | 12 chars; nunca `usu_id` interno |
| `call_id` | Sí | Lo genera el cliente (nanoid 12-32); úsalo en todos los eventos |
| `tipo` | Sí | `audio`, `video`, `walkie` |
| `ip` | No | Preferible omitir: el servidor usa `X-Forwarded-For`/`X-Real-IP` |
| `telefono` | No | Solo si el sistema externo lo usa como clave; es PII |
| `chat_id` Telegram | Solo Telegram | Lo guarda el servidor al vincular |

No envíes `usu_id` interno, tokens en URLs ni contenido en Telegram.

## 3. WSS: conexión y handshake GOST

- URL: `wss://<host>/ws/v1` (o `ws://127.0.0.1:8080/ws/v1` en local).
- Header: `Authorization: Bearer <access_token>` en el upgrade.
- **El handshake NO es RFC 6455 estándar**: el `Sec-WebSocket-Accept` se
  calcula con **Streebog-256** (GOST R 34.11-2012), no SHA-1:

```
accept = base64( Streebog256( Sec-WebSocket-Key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11" ) )
```

Usa la API **streaming** de `c_gost_encryption`
(`gost_hash_init/gost_hash_update/gost_hash_final`), que produce el digest
canónico RFC 6986. **No** uses el `gost_hash` one-shot reverso de stickers.

Consecuencia: `OkHttp`, `QWebSocket`, `websocat`, navegadores y librerías que
validan SHA-1 **no conectan**. Opciones:

1. Cliente WS propio con framing RFC 6455 + Streebog (recomendado).
2. Puente local que hable WS estándar hacia la app y WS GOST hacia el server.
3. Runtime parcheado (no recomendado).

En Android puedes portar el framing a Kotlin y llamar a `c_gost_encryption` por
JNI; en Qt, usar `QTcpSocket` + framing propio y el mismo núcleo C por FFI.

### Eventos
El servidor manda `welcome` al conectar. Luego:

```json
{
  "event": "call.incoming",
  "data": {
    "call_id": "abc123",
    "from_public": "NloXjs43tIJP",
    "from": "ChatA",
    "ip": "203.0.113.9",
    "tipo": "audio"
  }
}
```
```json
{
  "event": "call.accepted",
  "data": {
    "call_id": "abc123",
    "estado": "accepted",
    "from_public": "V1StGXR8_Z5j",
    "from": "ChatB",
    "tipo": "audio",
    "ip": "198.51.100.7"
  }
}
```
También: `call.rejected`, `call.ended`.

- Mantén un heartbeat/ping y reconecta con backoff (1, 2, 5, 10, 30 s).
- El token se valida **solo en el upgrade**; al reconectar usa un access token
  fresco. Si el refresh falla, pide login.
- `ws_online` en el servidor significa “hay un socket en `user:<usu_id>`”;
  app abierta sin WS **no** cuenta.

## 4. Flujo con la app abierta (sin Telegram)

```
Solicitante                         Servidor                         Receptor
    | POST /notify ------------------->|                                 |
    | {callee_public, call_id, tipo}   |-- WSS call.incoming ----------->|
    |                                  |                                 |
    |                                  |<-- POST /accept ----------------|
    |<-- WSS call.accepted ------------|    {call_id, ip?, telefono?}    |
    |                                  |                                 |
    | (media WebRTC: fase siguiente)   |                                 |
```

1. El solicitante crea `call_id` (nanoid) y llama:
```http
POST /api/v1/integrations/telegram/notify
Authorization: Bearer <token>
Content-Type: application/json

{ "usu_id_public": "<receptor>", "call_id": "<id>", "tipo": "audio" }
```
2. Respuesta:
```json
{ "success": true, "data": {
  "call_id": "<id>", "ws_online": true, "telegram_sent": false,
  "callee": "Nombre" } }
```
   - `ws_online=true`: el receptor tiene WSS; muestra “llamando”.
   - `ws_online=false` y `telegram_sent=true`: aviso por Telegram.
   - Ambos `false`: receptor no disponible; muestra error/missed.
3. El receptor recibe `call.incoming`; muestra pantalla de llamada entrante.
4. Acepta:
```http
POST /api/v1/integrations/telegram/accept
Authorization: Bearer <token>
Content-Type: application/json

{ "call_id": "<id>" }
```
   - `ip`/`telefono` son opcionales; si los omites, el servidor intenta
     `X-Forwarded-For`/`X-Real-IP`.
5. El solicitante recibe `call.accepted` con la IP del receptor.
6. Rechazar/terminar:
```http
POST /api/v1/integrations/telegram/reject   { "call_id": "<id>" }
POST /api/v1/integrations/telegram/end      { "call_id": "<id>" }
```
7. Si la app se abrió desde Telegram o perdió el evento:
```http
GET /api/v1/integrations/telegram/pending
```
   Devuelve llamadas ringing/aceptadas con `from_public`, `from`, `tipo`,
   `estado`, `ip` de la contraparte.

### Estados
```
idle -> outgoing_ringing -> accepted -> ended
                         -> rejected
idle -> incoming_ringing -> accepted -> ended
                         -> rejected
```
- Ringing expira a los 90 s; activa 6 h.
- El estado es **en memoria**: si el servidor reinicia, `/accept` puede dar
  409; muestra “llamada expirada”.
- `call_id` es generado por el cliente; no lo reutilices para otra llamada.

## 5. Telegram (opcional)

1. `POST /api/v1/integrations/telegram/link` (Bearer) → `{ code, url,
   expires_in }`.
2. Abre `url` (`https://t.me/<bot>?start=<code>`) y pulsa Start.
3. `GET /status` → `{ linked, telegram_username, telegram_name }`.
4. `POST /unlink` para desvincular.

El bot responde a `/start <código>`, `/status`, `/stop`, `/help`.

Si la app está cerrada, el mensaje de Telegram incluye
`cws://call/<call_id>`; el usuario lo toca y la app abre la pantalla de
llamada. **No hay ring automático** con la app cerrada sin FCM/APNs.

## 6. Deep link `cws://call/<call_id>`

### Android
`AndroidManifest.xml`:
```xml
<intent-filter>
  <action android:name="android.intent.action.VIEW" />
  <category android:name="android.intent.category.DEFAULT" />
  <category android:name="android.intent.category.BROWSABLE" />
  <data android:scheme="cws" android:host="call" />
</intent-filter>
```
- `onNewIntent`/`onCreate`: parsea `call_id`.
- Autentica, consulta `/pending` o llama `/accept` si el usuario confirma.
- **No auto-aceptes** desde el deep link: muestra pantalla y exige acción.

### Windows
Registra `HKEY_CLASSES_ROOT\cws` con `shell\open\command` apuntando al
ejecutable y pasa la URL como argumento. Usa una sola instancia y reenvía el
deep link a la ventana existente.

### Linux
`.desktop`:
```ini
MimeType=x-scheme-handler/cws;
Exec=/opt/cws/cws-client %u
```
Registra con `xdg-mime default cws.desktop x-scheme-handler/cws`.

## 7. Contrato HTTP

| Método | Ruta | Auth | Body |
|---|---|---|---|
| POST | `/api/v1/integrations/telegram/notify` | Bearer | `usu_id_public`, `call_id`, `tipo`, `ip?`, `telefono?` |
| POST | `/api/v1/integrations/telegram/accept` | Bearer | `call_id`, `ip?`, `telefono?` |
| POST | `/api/v1/integrations/telegram/reject` | Bearer | `call_id` |
| POST | `/api/v1/integrations/telegram/end` | Bearer | `call_id` |
| GET | `/api/v1/integrations/telegram/pending` | Bearer | — |
| POST | `/api/v1/integrations/telegram/link` | Bearer | — |
| GET | `/api/v1/integrations/telegram/status` | Bearer | — |
| POST | `/api/v1/integrations/telegram/unlink` | Bearer | — |

Errores: `400` datos inválidos, `401` token, `404` usuario/llamada,
`409` llamada no ringing/no es tuya, `503` bot Telegram deshabilitado (solo
`/link`). En `401` refresca el token y reintenta una vez.

## 8. Android

- **WSS**: cliente propio con framing RFC 6455 + Streebog; `OkHttp` estándar
  no sirve. Puedes reutilizar el framing en C y llamarlo por JNI.
- **Servicio**: `foregroundServiceType` para mantener WSS (y mic/cámara en la
  fase de media). Notificación persistente; canal aparte.
- **Doze/OEM**: pide exclusión de optimización de batería; aun así el sistema
  puede matar el proceso. Sin FCM, si muere no hay ring: Telegram es el
  fallback.
- **Permisos**: `INTERNET`, `POST_NOTIFICATIONS`; en la fase de media
  `RECORD_AUDIO`, `CAMERA`, `FOREGROUND_SERVICE_MICROPHONE/CAMERA`.
- **Deep link**: intent-filter arriba; singleTask/singleTop para reutilizar.
- **Almacenamiento**: access/refresh en Keystore/EncryptedSharedPreferences;
  CA privada + pinning en OkHttp/`network_security_config`.
- **Distribución**: APK firmado; sin Play Store; actualización self-hosted;
  el usuario debe permitir orígenes desconocidos.

## 9. Qt (Linux/Windows)

- **WSS**: `QWebSocket` valida SHA-1 y **no conecta**. Usa `QTcpSocket` con
  framing RFC 6455 propio + Streebog, o envuelve un cliente C en una API C
  expuesta a Qt.
- **Hilo/reentrancy**: el WS en un hilo/objeto dedicado; entrega eventos al
  hilo UI por señales/`QMetaObject::invokeMethod`.
- **Bandeja**: app en tray, autostart opcional, notificación local al
  `call.incoming`; sin FCM/WNS.
- **Deep link**: registro de `cws://` (Windows registry / Linux `.desktop`);
  una sola instancia; reenvía la URL a la ventana existente.
- **TLS**: CA privada + pinning; en Qt usa `QSslConfiguration` con la CA
  cargada y verificación de pin.
- **Almacenamiento**: DPAPI (Windows) / Secret Service o TPM (Linux) para
  tokens; no en texto plano.
- **Empaquetado**: MSI/portable y AppImage/deb; sin consola; auto-update
  self-hosted.

## 10. Señalización WebRTC 1:1 (audio)

Con la llamada **aceptada**, el servidor solo releva SDP/ICE; el media es
P2P (o TURN) y E2EE en el cliente.

| Método | Ruta | Uso |
|---|---|---|
| GET | `/api/v1/calls/:call_id` | Estado, rol y peer |
| GET | `/api/v1/calls/:call_id/ice` | STUN/TURN con credenciales temporales |
| POST | `/api/v1/calls/:call_id/sdp` | `{ "type":"offer|answer", "sdp":"..." }` |
| POST | `/api/v1/calls/:call_id/ice` | `{ "candidate":"...", "sdpMid":"...", "sdpMLineIndex":0, "usernameFragment":"..." }` |

- SDP/ICE solo se aceptan si la llamada está `accepted` (si no, `409`).
- El servidor releva al otro participante por WSS:
  - `call.sdp`: `{call_id, from_public, type, sdp}`
  - `call.ice`: `{call_id, from_public, candidate, sdpMid, sdpMLineIndex, usernameFragment}`
- `GET /ice` devuelve `ice_servers` con STUN y TURN (usuario/credencial
  temporales; expira). Refresca antes de `expires_in`.
- Si `ice_servers` viene vacío o con `warning`, el servidor no tiene
  STUN/TURN configurado. No hardcodees un STUN público: muestra el aviso y,
  si aplica, usa solo candidatos host (LAN). Sin TURN no hay fallback para
  NAT simétrico.
- `GET /calls/:call_id` devuelve `estado`, `role`, `peer_public`,
  `peer_name` y `peer_ip` (informativo; la ruta real la deciden los
  candidatos ICE).

Secuencia:
```
caller: notify -> accept (callee) -> GET /ice
caller: createOffer -> POST /sdp  -> callee recibe call.sdp
callee: createAnswer -> POST /sdp -> caller recibe call.sdp
ambos: onicecandidate -> POST /ice -> el otro recibe call.ice
media P2P/TURN; SFrame-GOST encima
```
- En el cliente, encola candidatos hasta tener `remoteDescription`.
- No uses `peer_ip` como dirección de media; usa ICE/STUN/TURN.
- El servidor no abre RTP ni descifra.

## 11. Media (fase siguiente)

Este puente **no** establece audio/video. La fase WebRTC usará:
- `call.accepted` como señal para iniciar SDP/ICE.
- `call_id` para correlacionar la sesión.
- ICE/STUN/TURN para la ruta; **no** uses la IP del evento como dirección de
  media: NAT, puertos y candidatos ICE son necesarios.
- E2EE GOST (SFrame-GOST) y Janus interno para grupo/walkie. La lib
  `c_gost_encryption` ya implementa Kuznyechik, AEAD y SFrame-GOST; falta
  integrarlo en el cliente WebRTC y auditarlo.
- El servidor ya expone ICE/TURN y relay SDP/ICE; el cliente debe conectar
  `RTCPeerConnection` y aplicar SFrame-GOST a los frames.

## 12. Seguridad

- No loguees tokens, `call_id` completos ni teléfonos.
- El deep link no lleva token: la app autentica y luego consulta/acepta.
- Verifica que el `from_public` del evento esté en tus contactos/llamada.
- No confíes en la IP del body; el proxy debe sanear `X-Forwarded-For`.
- Teléfono es PII: pídelo solo si el sistema externo lo exige; no lo muestres
  al otro extremo.
- Certificate pinning/CA privada; sin fallback a HTTP.

## 13. Checklist de aceptación

- [ ] App abierta + WSS: `call.incoming` sin Telegram/FCM.
- [ ] WSS caído: sin Telegram no hay ring; `/pending` sigue disponible.
- [ ] Telegram vinculado: aviso con `cws://call/<id>` y deep link abre la app.
- [ ] Aceptar/rechazar/terminar actualizan ambos lados.
- [ ] SDP/ICE se relevan solo en `accepted`; candidatos encolados hasta remoteDescription.
- [ ] `GET /calls/:id/ice` entrega STUN/TURN y se refresca antes de expirar.
- [ ] `call_id` no se reutiliza; 409 se muestra como expirada.
- [ ] Sin teléfono funciona; con teléfono opcional se mapea.
- [ ] `usu_id` interno nunca aparece en requests/respuestas/logs.
- [ ] Android: APK, foreground service, deep link, CA/pinning.
- [ ] Qt: WSS GOST, tray, deep link, CA/pinning.
- [ ] Pruebas de red: WS drop/reconnect, token expirado, app en background,
      servidor reiniciado (estado efímero).

## 14. Ejemplos curl

```bash
# Notificar (solicitante)
curl -X POST https://cws.sodastream.fun/api/v1/integrations/telegram/notify \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"usu_id_public":"NloXjs43tIJP","call_id":"abc123","tipo":"audio"}'

# Aceptar (receptor)
curl -X POST https://cws.sodastream.fun/api/v1/integrations/telegram/accept \
  -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -d '{"call_id":"abc123"}'

# Pendientes (app abierta desde Telegram)
curl https://cws.sodastream.fun/api/v1/integrations/telegram/pending \
  -H "Authorization: Bearer $TOKEN"
```
