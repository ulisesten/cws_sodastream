# Requerimiento FCM — notificaciones escalables

Actualizado: 2026-11-18. Complementa `docs/fcm_setup.md` (cómo configurar
Firebase) y `docs/endpoints.md`. Define **qué** notificaciones necesitamos,
**cómo** se modelan y **cómo** escalar a nuevos tipos sin tocar el emisor.

## Estado

- [IMPLEMENTADO][servidor] emisor FCM HTTP v1 con OAuth2, outbox y fan-out a
  dispositivos; `call.incoming`/`cancel`/`ended` y `message`, con retardo y
  cancelación por `delivery.ack`.
- [PARCIAL][servidor] catálogo básico; faltan preferencias mute/DND,
  `message.silent` real, `message.summary`, `read/deleted`, `call.missed`,
  rate limit, métricas y limpieza programada.
- [PENDIENTE][cliente] Firebase/PushService en el APK; el actual no lo incluye.
- [PENDIENTE][OPS] service account `FCM_CREDENTIALS`, `FCM_PROJECT_ID` y prueba
  E2E con dispositivo real.
- [PENDIENTE][servidor] cifrado de `usd_push_token` en reposo.
- Evidencia: `src/integrations/fcm`, `src/integrations/push`,
  `db_sodastream/push_outbox.sql`, commit `779f11b`; estado central en
  `docs/IMPLEMENTATION_STATUS.md`.

## 1. Objetivo

Notificar al usuario en tiempo real cuando el WSS no está disponible (app en
segundo plano, matada, pantalla bloqueada), con tres familias:

1. **Llamadas**: llamada entrante, cancelada, perdida, finalizada.
2. **Mensajes**: mensaje normal (con sonido) y mensaje silencioso (sin sonido).
3. **Sincronización silenciosa**: datos sin notificación visible (leídos,
   borrados, reacciones, refresco de inbox).

Requisitos transversales: bajo consumo, sin duplicados, multi-dispositivo,
respeto de silencio/DND, privacidad, y **catálogo de tipos** para añadir nuevos
sin cambiar el emisor.

## 2. Principios de diseño

- **Data-only** en lo posible: el servidor envía `data` y la app construye la
  notificación (control de canales, full-screen, idioma y privacidad).
  Para llamadas el mensaje debe ser de **alta prioridad**.
- **Catálogo de tipos**: cada tipo declara canal, prioridad, TTL, sonido,
  full-screen, visibilidad, collapse key y handler de cliente. Añadir un tipo
  = añadir una entrada de catálogo (+ handler de UI si es visible).
- **Idempotencia**: `event_id`/`dedupe_key` (p. ej. `call_id`, `msg_id`) y
  outbox; FCM puede entregar duplicados.
- **Entrega WSS + ack**: el servidor no decide por `ws_online`; encola FCM con
  retardo (`FCM_ACK_TIMEOUT_MS`, default 2 s). Si el cliente envía
  `delivery.ack` con `event_id`+`device_id`, se cancela el push de ese
  dispositivo; si no, FCM lo envía. Un socket vivo sin ack **no** cuenta como
  entregado.
- **Colapso**: `collapse_key` por llamada/chat para no apilar notificaciones.
- **Multi-dispositivo**: fan-out a todos los dispositivos activos del usuario;
  excluir el dispositivo emisor cuando se conozca.
- **Privacidad por defecto**: no incluir SDP, claves, tokens ni historial; el
  cuerpo puede ser un preview corto o un texto genérico (“Nuevo mensaje”).
- **Silencio**: el servidor decide `silent` según mute/DND; el cliente puede
  degradar a silencioso si la app está en primer plano o el chat está abierto.
- **Proveedor abstracto**: FCM ahora; APNs/WebPush después con el mismo
  contrato (`PushProvider`).

## 3. Taxonomía de notificaciones

| Tipo | Visible | Prioridad | Canal Android | Sonido | Full-screen | TTL | Collapse |
|---|---|---|---|---|---|---|---|
| `call.incoming` | Sí | high | `calls_incoming` | ringtone | Sí | 60 s | `call:<call_id>` |
| `call.cancel` | No (data) | high | — | — | — | 60 s | `call:<call_id>` |
| `call.missed` | Sí (local) | normal | `calls_missed` | default | No | 1 h | `call:<call_id>` |
| `call.ended` | No (data) | normal | — | — | — | 10 min | `call:<call_id>` |
| `message` | Sí | normal | `messages` | default | No | 1 día | `chat:<chat_id>` |
| `message.silent` | Sí | normal | `messages_silent` | ninguno | No | 1 día | `chat:<chat_id>` |
| `message.summary` | Sí | low | `messages` | ninguno | No | 1 día | `chat_summary` |
| `message.deleted` | No (data) | normal | — | — | — | 10 min | `chat:<chat_id>` |
| `message.read` | No (data) | normal | — | — | — | 10 min | `chat:<chat_id>` |
| `sync.inbox` | No (data) | normal | — | — | — | 1 día | `inbox` |

Notas:

- **`call.cancel`/`call.ended`** deben retirar la notificación de llamada y
  evitar que el teléfono siga sonando. Son data-only de alta prioridad.
- **`call.missed`** se genera si el ringing expira; puede ser local (la app ya
  lo muestra) o push si la app estaba matada.
- **`message.silent`** es una notificación visible **sin sonido ni vibración**;
  úsala para chats silenciados o DND. No confundir con `sync.*` (sin
  notificación).
- **`message.summary`** agrupa N chats en una notificación resumen; sirve para
  escalar (evita 20 notificaciones sueltas).
- **`sync.*`** no muestra nada: la app refresca inbox/estado al despertar.

## 4. Contrato de payload

Común (data-only, < 4 KB):

```json
{
  "v": 1,
  "type": "message",
  "event_id": "evt_01J...",
  "ts": 1794900000,
  "dedupe_key": "msg:12345",
  "collapse_key": "chat:Ny5IiLev59ia",
  "priority": "normal",
  "ttl": 86400,
  "silent": false,
  "chat_id": "Ny5IiLev59ia",
  "msg_id": 12345,
  "sender": "ChatA",
  "from_public": "Ny5IiLev59ia",
  "title": "ChatA",
  "body": "Hola",
  "deep_link": "cws://chat/Ny5IiLev59ia"
}
```

Llamada:

```json
{
  "v": 1,
  "type": "call.incoming",
  "event_id": "evt_...",
  "call_id": "6fJ...",
  "sender": "ChatB",
  "from_public": "tejRZUY7HpSu",
  "tipo": "video",
  "e2ee": true,
  "priority": "high",
  "ttl": 60,
  "silent": false,
  "deep_link": "cws://call/6fJ..."
}
```

Reglas:

- `type` es obligatorio y versionado con `v`.
- **Claves reservadas por FCM**: `data` no puede incluir `from`, `message_type`
  ni claves que empiecen por `google.`/`gcm.`; FCM responde
  `400 Invalid data payload key`. Por eso el nombre del emisor viaja en
  `sender` (no en `from`).
- `event_id` único (ULID/UUID) para deduplicar; `dedupe_key` estable por
  entidad (llamada/mensaje) para reemplazos.
- `priority`: `high` **solo** llamadas/cancelaciones; el resto `normal`.
- `ttl`: segundos; llamadas 60, mensajes 1 día, sync 1 día.
- `silent`: true para `message.silent`; el cliente usa el canal silencioso.
- `deep_link`: `cws://call/<id>` o `cws://chat/<public_id>`; validar formato.
- No incluir SDP, claves, tokens, historial ni datos médicos/financieros. El
  `body` es un preview corto; para privacidad puede ser “Nuevo mensaje”.
- Para llamadas, el payload debe ser **data-only** para que la app construya
  el full-screen intent; si se usa `notification`, FCM la muestra y no llama a
  `onMessageReceived` en background.

## 5. Comportamiento por tipo (cliente)

### Llamada

1. `onMessageReceived` → validar `type`, `call_id`, `from_public`.
2. Comprobar que no haya una llamada activa/terminal duplicada (`call_id`).
3. Notificación `calls_incoming` con `fullScreenIntent` a
   `cws://call/<id>`, categoría `CALL`, `VISIBILITY_PUBLIC`, `ongoing`,
   `timeoutAfter(60s)`.
4. `call.cancel`/`call.ended` → cancelar la notificación y el ring local.
5. Si el usuario la abre, `recoverCall(call_id)` consulta `/pending`/`/calls/:id`
   y hace fail-closed si no existe.
6. Android 13+: permiso de notificaciones; Android 14+: permiso de
   notificaciones a pantalla completa. Si falta, mostrar heads-up y avisar.

### Mensaje normal

1. Si el chat está abierto y la app en primer plano → no notificar.
2. Si no, notificación en `messages`:
   - `setGroup("chat:<chat_id>")`, `setGroupSummary(false)`.
   - ID estable por chat (`base + chat_id.hashCode()`).
   - `contentIntent` → `open_chat=<chat_id>`.
   - `setAutoCancel(true)`, `VISIBILITY_PRIVATE` + `setPublicVersion`.
3. Varios mensajes del mismo chat: reemplazar la notificación (collapse) y
   mostrar el último; opcional “N mensajes”.
4. Al abrir el chat: cancelar la notificación de ese chat.

### Mensaje silencioso

- Mismo flujo que `message`, pero canal `messages_silent`
  (`IMPORTANCE_LOW`, sin sonido/vibración) y `setOnlyAlertOnce(true)`.
- Se usa si:
  - el chat está silenciado por el usuario;
  - el usuario está en DND;
  - la app está en primer plano pero el chat no está abierto
    (configurable; por defecto sí notificar en silencio).
- No usar `setSilent(true)` con un canal audible; el canal manda.

### Sync silencioso

- `message.read`, `message.deleted`, `sync.inbox`: no crear notificación.
- Actualizar el inbox/estado local y, si el mensaje estaba notificado,
  cancelar/actualizar la notificación correspondiente.
- No hacer sonar/vibrar ni despertar la pantalla.

## 6. Escalabilidad

### 6.1 Catálogo en el servidor

Tabla/archivo `notification_types` (o YAML) con:

```yaml
message.silent:
  channel: messages_silent
  visible: true
  priority: normal
  ttl: 86400
  sound: none
  full_screen: false
  collapse: "chat:{chat_id}"
  handler: message
```

El emisor solo resuelve el tipo y aplica defaults. **Añadir un tipo nuevo no
requiere cambiar el código del emisor**; solo el catálogo y, si es visible, el
handler del cliente.

### 6.2 Fan-out

1. Evento de dominio (`call.ringing`, `dm.created`, `dm.read`, …).
2. `push_service` consulta dispositivos activos del usuario
   (`opr_usuarios_devices`, `usd_activo=1`, token no vacío).
3. Aplica preferencias (mute/DND/silent) y `collapse_key`.
4. Encola en outbox (`push_outbox`) con `event_id`/`dedupe_key`.
5. Worker envía a FCM HTTP v1 por dispositivo; reintenta con backoff;
   elimina tokens `UNREGISTERED`/`INVALID_ARGUMENT`.
6. Métricas por tipo/estado.

### 6.3 Preferencias y silencio

- Por usuario/chat: `notify_mode` = `all | mentions | silent | none`.
- Por tipo: llamadas siempre (salvo bloqueo), mensajes según chat.
- DND del sistema/dispositivo: Android suprime el sonido; el servidor puede
  enviar `message.silent` si el usuario lo configuró.
- El cliente puede degradar a silencioso si el chat está abierto.

### 6.4 Agrupación y batching

- `collapse_key` por chat/llamada.
- `message.summary` cuando hay > N chats con no leídos (p. ej. 5) o > X
  notificaciones por minuto; una sola notificación resumen con “N mensajes en
  M chats”.
- Rate limit por usuario/chat (p. ej. 1 push cada 10 s con agregación) para
  evitar spam y ahorrar batería.
- **No se decide por `ws_v1_user_online`.** Se emite WSS, se encola FCM con
  retardo (`FCM_ACK_TIMEOUT_MS`) y el cliente confirma con `delivery.ack`; si
  no hay ack, sale FCM. Un socket vivo sin ack no cuenta como entregado.

### 6.5 Abstracción de proveedor

```
PushProvider (interfaz)
  - fcm (Android)
  - apns (iOS, futuro)
  - webpush (web, futuro)
```

El payload de dominio es común; cada provider lo adapta. El catálogo define
`android_channel`, `apns_category`, etc.

### 6.6 Outbox e idempotencia

- Tabla `push_outbox` con `event_id` único, `user_id`, `device_id`, `type`,
  `payload`, `status`, `attempts`, `next_retry_at`.
- `UNIQUE(user_id, event_id, device_id)` para no duplicar.
- Worker con `SELECT ... FOR UPDATE SKIP LOCKED` (o equivalente) para escalar
  horizontalmente.
- Retención corta (p. ej. 7 días) y limpieza.

## 7. Configuración del servidor

```env
FCM_ENABLED=true
FCM_PROJECT_ID=ctburst
FCM_CREDENTIALS=/etc/cws/fcm-service-account.json
FCM_DEFAULT_TTL=86400
FCM_CALL_TTL=60
FCM_MAX_DEVICES_PER_USER=10
FCM_RATE_PER_MINUTE=60
FCM_DRY_RUN=false
```

- Credenciales **fuera del repo** y `chmod 600`.
- OAuth2 con caché del access token (renovar 5 min antes).
- HTTP v1 `messages:send`; `data-only`; `android.priority=high` solo llamadas.
- Si `FCM_DRY_RUN=true`, loguear sin enviar (staging).

## 8. Seguridad y privacidad

- Service account con permisos mínimos (Firebase Cloud Messaging API).
- No incluir secretos/SDP/claves/historial en el payload.
- Tokens push cifrados en reposo (o al menos BD restringida) y borrados al
  logout/`UNREGISTERED`.
- Limitar dispositivos por usuario y rate limit por IP/usuario.
- Validar `type`/`deep_link` en el cliente; ignorar tipos desconocidos con log.
- Notificaciones de mensajes `VISIBILITY_PRIVATE` + `publicVersion` genérica.
- Llamadas: `VISIBILITY_PUBLIC` (nombre) solo si el usuario lo permite.
- No usar el push para transportar media ni claves E2EE.

## 9. Requisitos de cliente (Android)

- `FirebaseMessagingService` con registro de handlers por `type`
  (mapa/`when` extensible) y fallback para desconocidos.
- Canales: `calls_incoming` (HIGH + ringtone), `calls_missed` (DEFAULT),
  `messages` (DEFAULT), `messages_silent` (LOW, sin sonido), `calls_service`
  (LOW, FGS).
- Permisos: `POST_NOTIFICATIONS`, `USE_FULL_SCREEN_INTENT`; comprobar y guiar
  al usuario si están denegados.
- `PendingIntent` inmutables con deep link; validar `call_id`/`chat_id`.
- IDs de notificación estables por chat/llamada; cancelar al abrir/leer.
- No notificar el chat abierto ni los mensajes propios.
- Multi-dispositivo: el mismo usuario puede recibir en varios; el cliente
  ignora eventos ya aplicados (`event_id`/`msg_id`).

## 10. Implementación en la API (estado 2026-11-18)

Implementado:

1. Módulo `fcm` con OAuth2 (JWT RS256 + token cache), HTTP v1, data-only,
   `android.priority/ttl/collapse_key`, dry-run y detección de tokens
   inválidos (`src/integrations/fcm`).
2. Outbox `opr_push_outbox` + `procPushOutboxProc/Cons` y worker en
   `src/integrations/push` con reintentos/backoff.
3. Consulta de tokens activos (`procUsersDevicesCons` tipo 4,
   `users_devices_push_tokens`).
4. `telegram_notify_bridge`: emite WSS, encola FCM con retardo y espera
   `delivery.ack`; Telegram solo si FCM no encoló.
5. `reject`/`end`: `call.cancel`/`call.ended` a la contraparte.
6. `POST /conversations/:public/messages`: `message` con retardo y
   `delivery.ack` para cancelar.

Pendiente:

- Preferencias mute/DND, `message.silent` real, `message.summary`,
  `message.read`/`deleted` sync y `call.missed` por timeout.
- El cliente **implementa** `delivery.ack` en `RealtimeClient` (se envía al
  recibir `dm`/`call.incoming` con `event_id`, antes de despachar al UI, con el
  `device_id` estable y deduplicación); sin ack, FCM se envía aunque el
  WSS haya entregado el evento.
- Rate limit `FCM_RATE_PER_MINUTE`, métricas por tipo y limpieza programada.
- Cifrado de `usd_push_token` en reposo.
- Cliente: el APK actual no incluye Firebase; ver `docs/fcm_setup.md`.

Requiere aplicar los dos scripts canónicos
`db_sodastream/usuarios_devices.sql` (incluye `procPushTokensCons` y tipo 4)
y `db_sodastream/push_outbox.sql`, configurar `FCM_*` y el service account.
No hay parches SQL aparte; ambos son idempotentes y se auto-verifican.

## 11. Matriz de pruebas

| Caso | Esperado |
|---|---|
| App abierta en el chat | Sin notificación |
| App en background, otro chat | Notificación `messages` con sonido |
| Chat silenciado | `messages_silent`, sin sonido/vibración |
| App matada, llamada | Full-screen + ringtone; aceptar abre la llamada |
| Llamada cancelada | Se retira la notificación y deja de sonar |
| Llamada perdida | `call.missed` una sola vez |
| Varios mensajes mismo chat | Una notificación actualizada (collapse) |
| Muchos chats | `message.summary` |
| `message.read` | Cancela la notificación del chat |
| Token rotado | El nuevo token reemplaza al viejo |
| Token inválido | Se borra de la BD; no reintenta infinito |
| Sin permiso de notificaciones | Sin crash; guía para conceder |
| Android 14 sin full-screen | Heads-up + aviso de ajuste |
| Duplicado `event_id` | No duplica notificación |
| Tipo desconocido | Ignorado con log, sin crash |

## 12. Criterios de aceptación

- Llamadas y mensajes llegan con la app matada/bloqueada en ≤ 5 s (red normal).
- Un mensaje silencioso no suena ni vibra, pero aparece en la bandeja.
- No hay notificaciones duplicadas por reintentos/eventos repetidos.
- El servidor puede añadir un tipo nuevo solo con catálogo + handler.
- Tokens inválidos se limpian; el fan-out no bloquea el hilo de la petición.
- Métricas por tipo y tasa de fallo disponibles.
