# FCM (push) para llamadas y mensajes

Actualizado: 2026-11-17. Complementa `docs/endpoints.md` (dispositivos) y
`docs/telegram_client.md` (§8 Android).

Objetivo: recibir `call.incoming` y mensajes cuando el WSS está caído o la app
está en segundo plano, sin depender del bot de Telegram.

> Requerimiento funcional completo (tipos, payloads, silencio, agrupación y
> escalabilidad): `docs/fcm_notifications.md`.

## Estado (2026-11-18)

- [IMPLEMENTADO][servidor] emisor FCM/outbox, `/users/device` multi-dispositivo,
  consulta de tokens activos y cancelación por `delivery.ack`.
- [PARCIAL][servidor] tipos/preferencias/rate limit/métricas pendientes (ver
  `fcm_notifications.md` §10).
- [PENDIENTE][cliente] Firebase/PushService en el APK; el actual no incluye
  Firebase (nota final).
- [PENDIENTE][OPS] service account (`FCM_CREDENTIALS`), proyecto, deploy y
  prueba con dispositivo real.
- [PENDIENTE][servidor] cifrado de `usd_push_token` en reposo.
- Estado central: `docs/IMPLEMENTATION_STATUS.md`.

## 0. SQL requerido y verificación

El servidor consulta tokens con `procUsersDevicesCons @tipoConsulta=4` y encola
en `opr_push_outbox`. Si el SP desplegado es una versión vieja, no devuelve
tokens y el log muestra `push: ... devices=1 enqueued=0` (la fila `devices=1`
es la fila `success=false` del SP, no un dispositivo).

Solo existen **dos** archivos canónicos. No hay parches aparte: todo cambio
va en estos archivos y se aplican completos, con un login con DDL, en orden:

```bash
sqlcmd -S <server> -d soda_stream -i db_sodastream/usuarios_devices.sql
sqlcmd -S <server> -d soda_stream -i db_sodastream/push_outbox.sql
```

Son idempotentes: crean/actualizan columnas faltantes y terminan con una
verificación que **falla** (`RAISERROR`) si falta algo. Aplicar una copia
vieja deja el SP anterior y la verificación lo delata.

`usuarios_devices.sql` instala:

- `procUsersDevicesProc` (alta/desactiva/borra dispositivos).
- `procUsersDevicesCons` con `@PUSH_TOKENS=4` (compatibilidad).
- `procPushTokensCons` (SP dedicado que usa el servidor; una copia vieja de
  `procUsersDevicesCons` ya no lo puede romper).

Verificación manual:

```sql
-- Ambos deben devolver usd_device_id y usd_push_token
-- (0 filas si el usuario no tiene token registrado).
EXEC dbo.procPushTokensCons @usu_id=<id>;
EXEC dbo.procUsersDevicesCons @tipoConsulta=4, @usd_id_usuario=<id>;

-- Deben devolver 'SI'.
SELECT CASE WHEN OBJECT_ID('dbo.procPushTokensCons','P') IS NOT NULL
                 THEN 'SI' ELSE 'NO' END AS dedicado;
SELECT CASE WHEN OBJECT_DEFINITION(OBJECT_ID('dbo.procUsersDevicesCons'))
                 LIKE '%@PUSH_TOKENS%' THEN 'SI' ELSE 'NO' END AS tipo4;
SELECT CASE WHEN OBJECT_DEFINITION(OBJECT_ID('dbo.procPushOutboxProc'))
                 LIKE '%@po_delay_s%' THEN 'SI' ELSE 'NO' END AS outbox_delay;
```

Prueba segura del outbox (no envía nada porque el retardo es de 1 hora):

```sql
EXEC dbo.procPushOutboxProc @tipoProceso=1, @po_event_key='selftest:enqueue',
    @po_id_usuario=<id>, @po_device_id='<device>', @po_token='fake',
    @po_type='message', @po_payload='{"v":"1"}', @po_delay_s=3600;
EXEC dbo.procPushOutboxProc @tipoProceso=7, @po_event_key='selftest:enqueue',
    @po_id_usuario=<id>, @po_device_id='<device>';
```

Log esperado al enviar un mensaje o llamada:

```text
push: type=message event=msg:<id> devices=1 enqueued=1 delay=2s
push: worker claim=1
push: sent type=message device=<device> code=200
```

## 1. Cliente Android

### 1.1 Firebase

1. Crear un proyecto en <https://console.firebase.google.com>.
2. Añadir una app Android con package **`com.sodastream.mobile`**.
3. Descargar `google-services.json` y copiarlo a `mobile/app/google-services.json`
   (no commitearlo si el repo es público; usar el de cada build).
4. En Firebase → Configuración del proyecto → Cuentas de servicio, generar la
   clave privada para el **servidor** (no para la app).

### 1.2 Gradle

Raíz (`build.gradle.kts`):

```kotlin
plugins {
    id("com.google.gms.google-services") version "4.4.2" apply false
}
```

`app/build.gradle.kts`:

```kotlin
plugins {
    id("com.google.gms.google-services")
}

dependencies {
    implementation(platform("com.google.firebase:firebase-bom:33.7.0"))
    implementation("com.google.firebase:firebase-messaging")
}
```

### 1.3 Manifest

```xml
<uses-permission android:name="android.permission.POST_NOTIFICATIONS" />
<uses-permission android:name="android.permission.USE_FULL_SCREEN_INTENT" />

<service
    android:name=".service.PushService"
    android:exported="false">
    <intent-filter>
        <action android:name="com.google.firebase.MESSAGING_EVENT" />
    </intent-filter>
</service>
```

`POST_NOTIFICATIONS` ya se pide en la app (Android 13+). En Android 14+ el
usuario debe permitir “notificaciones a pantalla completa” para apps de
llamadas; añadir un aviso en ajustes si está denegado.

### 1.4 Servicio

```kotlin
class PushService : FirebaseMessagingService() {
    override fun onNewToken(token: String) {
        // Guardar token y registrarlo cuando haya sesión.
        PushTokenStore(applicationContext).save(token)
        PushRegistrar.registerIfPossible(applicationContext)
    }

    override fun onMessageReceived(message: RemoteMessage) {
        val data = message.data
        if (data["type"] == "call") {
            // Notificación de llamada con full-screen intent y deep link:
            // cws://call/<call_id>
            CallNotifications.showIncomingFromPush(applicationContext, data)
        } else {
            CallNotifications.showMessage(applicationContext, data)
        }
    }
}
```

- Para llamadas usar **notification + data** (`channel_id=calls`,
  prioridad alta, sonido/vibración, `fullScreenIntent` a
  `MainActivity` con `cws://call/<id>`, `setTimeoutAfter(60s)`).
- Para mensajes, **data-only** y que la app actualice el inbox.
- El `call_id` y `from_public` van en `data`; la app abre la llamada con
  `recoverCall(callId)` / `GET /calls/:id` y el WSS.
- Un `data` de llamada debe ser pequeño (<4 KB): `type`, `call_id`,
  `from`, `from_public`, `tipo`, `e2ee`.

### 1.5 Registro del token

1. Tras el login, obtener el token:
   ```kotlin
   FirebaseMessaging.getInstance().token.addOnCompleteListener { task ->
       val token = task.result
       // POST /api/v1/users/device con device_id + push_token
   }
   ```
2. `POST /api/v1/users/device`:
   ```json
   { "device_id": "<uuid estable>", "plataforma": 1,
     "nombre": "Pixel 7", "push_token": "<fcm-token>" }
   ```
3. En `onNewToken`, repetir el `POST` (el token rota).
4. Al cerrar sesión: `DELETE /api/v1/users/device` con `device_id` (o sin él
   para logout-all).
5. El `device_id` debe ser el mismo que usa `DeviceIdStore` (UUID persistido).

### 1.6 Prueba

```bash
adb logcat | grep -iE "firebase|FCM|PushService"
```

- Firebase Console → Messaging → “Send test message” al token.
- Data de prueba:
  ```json
  { "type": "call", "call_id": "abc123", "from": "ChatB",
    "from_public": "tejRZUY7HpSu", "tipo": "video", "e2ee": "1" }
  ```
- Verificar: notificación con pantalla completa, sonido, deep link y que la
  app abre la llamada.

## 2. Servidor (C)

### 2.1 Credenciales

- Guardar el JSON de la cuenta de servicio fuera del repo:
  `FCM_CREDENTIALS=/etc/cws/fcm-service-account.json`
  `FCM_PROJECT_ID=<project-id>`
- Nunca commitear el JSON.

### 2.2 Token OAuth2

1. Construir un JWT `RS256` con `iss=client_email`, `scope=
   https://www.googleapis.com/auth/firebase.messaging`, `aud=
   https://oauth2.googleapis.com/token`, `iat`, `exp` (1 h).
2. `POST https://oauth2.googleapis.com/token`
   `grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer&assertion=<jwt>`.
3. Cachear el access token hasta ~5 min antes de expirar.

### 2.3 Envío HTTP v1

```http
POST https://fcm.googleapis.com/v1/projects/<FCM_PROJECT_ID>/messages:send
Authorization: Bearer <access_token>
Content-Type: application/json

{
  "message": {
    "token": "<push_token>",
    "data": {
      "type": "call",
      "call_id": "<call_id>",
      "from": "<nombre>",
      "from_public": "<public_id>",
      "tipo": "video",
      "e2ee": "1"
    },
    "android": {
      "priority": "high",
      "ttl": "60s",
      "notification": {
        "channel_id": "calls",
        "sound": "default"
      }
    },
    "apns": {
      "headers": { "apns-priority": "10" },
      "payload": { "aps": { "sound": "default", "content-available": 1 } }
    }
  }
}
```

- Para llamadas: **data-only** con `priority=high` y `ttl=60s`; la app
  (`PushService`) construye la notificación con `fullScreenIntent` y el deep
  link. Evita que FCM muestre una notificación sin pantalla completa cuando el
  proceso está muerto.
- Para mensajes: data-only; no usar `priority=high` salvo urgencia.
- Si se usa `notification` en el payload, FCM la muestra y **no** llama a
  `onMessageReceived` en background; el full-screen intent tendría que ir en
  el payload y no se puede personalizar igual.
- En Android 13+ sin permiso de notificaciones el token sigue válido, pero la
  notificación no se muestra: la app debe pedir el permiso.

### 2.4 Integración con el puente de llamadas

En `telegram_notify_bridge` (o equivalente), cuando `ws_online == false`:

1. Consultar `opr_usuarios_devices` por `usu_id` con `usd_activo=1` y
   `push_token` no vacío.
2. Enviar a **todos** los dispositivos (multi-dispositivo) o excluir el
   `device_id` del solicitante si se conoce.
3. Si FCM responde `UNREGISTERED`/`INVALID_ARGUMENT`, limpiar el token.
4. Mantener Telegram como fallback (`telegram_sent`); añadir `push_sent` al
   outcome si se quiere distinguir.
5. Rate limit por usuario y por llamada; no enviar dos push para el mismo
   `call_id`.

### 2.5 Seguridad y privacidad

- No incluir SDP, claves, tokens ni texto de mensajes en el push.
- Solo `call_id`, tipo, nombre público y `from_public`.
- El push no sustituye al WSS: la app debe reconectar y pedir
  `GET /calls/:id` / `/pending` para validar el estado real.
- Registrar métricas (entregados/fallidos) sin payload.

## 3. Orden de despliegue

1. Aplicar las migraciones de dispositivos (`usuarios_devices.sql`) y
   desplegar el endpoint `/users/device` (ya en la API nueva).
2. Configurar `FCM_PROJECT_ID` + `FCM_CREDENTIALS` en el servidor.
3. Añadir el servicio `PushService`, el plugin de Google y
   `google-services.json` en la app.
4. Registrar el token tras el login y probar una llamada con la app en
   segundo plano y el WSS caído.
5. Mantener Telegram como fallback hasta validar FCM en ambos dispositivos.

## Nota

El APK actual **no** incluye Firebase. Los pasos 1.2–1.5 requieren añadir la
dependencia y el `google-services.json`; sin ese archivo el build falla, por
eso no se dejan las dependencias activas en el repo.
