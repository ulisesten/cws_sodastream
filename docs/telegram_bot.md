# Integración Telegram + puente de llamadas

Implementación en `src/integrations/telegram/` y
`src/api/v1/routes/integrations/`.

## Qué hace
- Vincula una cuenta de la app con un chat privado de Telegram usando un
  **código de un solo uso**.
- Cuando se crea una llamada, emite `call.incoming` por **WSS** si la app está
  abierta y, si no hay socket activo (o `TELEGRAM_NOTIFY_ALWAYS=1`), manda un
  mensaje de Telegram con `cws://call/<call_id>`.
- Expone una API HMAC para que un sistema externo (IP/teléfono/ID) mapee
  usuarios y notifique llamadas, sin enviar contenido.
- Sin FCM/APNs/WNS. Telegram es solo notificación/deep link; no transporta
  media ni contenido.

## Requisitos
0. Dependencia de build: **libcurl** (`libcurl-devel`/`libcurl4-openssl-dev`).
   El servidor necesita salida HTTPS a `api.telegram.org`.
1. Crear un bot con **@BotFather** y obtener `TELEGRAM_BOT_TOKEN` y
   `TELEGRAM_BOT_USERNAME`.
2. Aplicar `db_sodastream/video_llamadas_integraciones.sql` con un usuario con permiso
   `CREATE TABLE`/`CREATE PROCEDURE` (el usuario de la app normalmente no lo
   tiene).
3. Configurar `.env` (el token y el username son del bot):
```dotenv
TELEGRAM_BOT_TOKEN=123456:ABC...
TELEGRAM_BOT_USERNAME=mi_bot
TELEGRAM_API_BASE=https://api.telegram.org
TELEGRAM_POLL_TIMEOUT=20
TELEGRAM_NOTIFY_ALWAYS=0
TELEGRAM_LINK_TTL_MIN=15
TELEGRAM_CALL_URI=cws
EXTERNAL_API_SECRET=<secreto-largo-aleatorio>
```
`TELEGRAM_API_BASE` solo se cambia para un mock/pruebas locales.
`TELEGRAM_NOTIFY_ALWAYS=0`: si la app está online solo WSS; `1`: también
Telegram aunque esté online.

## Flujo de vinculación
1. App autenticada: `POST /api/v1/integrations/telegram/link`.
2. Abre la URL `https://t.me/<bot>?start=<code>` y pulsa **Start**.
3. El bot recibe `/start <code>`, valida el código, guarda
   `chat_id ↔ usu_id` y responde.
4. `GET /status` muestra `linked`; `POST /unlink` borra el vínculo.

Comandos del bot:
- `/start <código>`: vincular.
- `/status`: ver vínculo.
- `/stop`: desvincular.
- `/help`: ayuda.

## Notificación desde la app (solicitante)
`POST /api/v1/integrations/telegram/notify` (Bearer):
```json
{
  "usu_id_public": "NloXjs43tIJP",
  "call_id": "call123",
  "tipo": "audio",
  "ip": "203.0.113.9",
  "telefono": "+5215550001"
}
```
- `ip`/`telefono` son opcionales: el servidor toma la IP de
  `X-Forwarded-For`/`X-Real-IP` si existe y, si no, del body. Se guardan en el
  mapeo externo (IP/teléfono → usuario).
- Crea un estado de llamada **en memoria** (ringing) con `call_id`,
  solicitante y receptor.
- Emite WSS `call.incoming` con `{call_id,from_public,from,ip,tipo}`
  (la IP del solicitante sale de `X-Forwarded-For`/`X-Real-IP` o del body).
- Si no hay WS (o `NOTIFY_ALWAYS=1`) envía Telegram:
  `Llamada de <nombre> (<tipo>)` + `cws://call/<call_id>`.
- La app debe registrar el esquema `cws://` (Android intent-filter,
  Linux/Windows custom URL handler) para abrir la llamada.

> Si la app está cerrada, **no hay ring automático**: el usuario recibe el
> mensaje de Telegram y toca el enlace. Para ring automático haría falta
> FCM/APNs (aprobado para después).

## Aceptar / rechazar / terminar (receptor)
El receptor, ya autenticado, confirma y aporta su IP/teléfono al aceptar:

`POST /api/v1/integrations/telegram/accept` (Bearer):
```json
{ "call_id": "call123", "ip": "198.51.100.7", "telefono": "+5215550002" }
```
- Valida que el `call_id` esté ringing y que el usuario sea el receptor.
- Guarda IP/teléfono del receptor en el mapeo externo.
- Emite al solicitante WSS `call.accepted`
  `{call_id, estado:"accepted", from_public, from, tipo, ip}`.
- Devuelve `caller_public`, `caller_name`, `tipo` y `estado`.

`POST /api/v1/integrations/telegram/reject` (Bearer):
```json
{ "call_id": "call123" }
```
Emite `call.rejected` al solicitante.

`POST /api/v1/integrations/telegram/end` (Bearer, cualquiera):
```json
{ "call_id": "call123" }
```
Emite `call.ended` al otro extremo.

`GET /api/v1/integrations/telegram/pending` (Bearer): lista llamadas
ringing/aceptadas del usuario (para cuando abrió la app desde Telegram),
con `from_public`, `from`, `tipo`, `estado` e `ip` de la contraparte.

> El estado de llamada es **efímero** (ringing 90 s, activa 6 h) y se pierde al
> reiniciar. El módulo de llamadas persistente validará `call_id` y armará el
> media; esto es el puente de notificación/vinculación.

## API externa (HMAC)
Headers obligatorios en cada request:
```
X-Integration-Timestamp: <unix segundos>
X-Integration-Nonce: <aleatorio, 8-128 chars>
X-Integration-Signature: <hex HMAC-Streebog-256(secret, ts + "\n" + nonce + "\n" + body)>
```
Ventana ±300 s; nonces reutilizados se rechazan.

### `POST /api/v1/integrations/external/link`
```json
{ "usu_id_public": "NloXjs43tIJP", "telefono": "+521...", "ip": "203.0.113.5", "id_externo": "CRM-1" }
```
Guarda los mapeos que vengan (tipo 1 teléfono, 2 IP, 3 ID externo). Normalmente
ya se llenan solos con `/telegram/notify` (solicitante) y `/telegram/accept`
(receptor); este endpoint sirve para precargarlos si el sistema externo ya
tiene el `usu_id_public`.

### `POST /api/v1/integrations/external/notify`
```json
{ "telefono": "+521...", "call_id": "call123", "tipo": "audio", "nombre": "PBX" }
```
Acepta `telefono`, `ip`, `id_externo` o `usu_id_public`; resuelve el usuario y
hace el mismo puente WSS/Telegram. No envía contenido. Sirve para notificar
desde una PBX/CRM cuando ya existe el mapeo (creado por la app al llamar/aceptar).

## Seguridad
- Códigos de vinculación: 32 chars, TTL corto, un solo uso.
- Solo chat privado; `/stop` borra el vínculo.
- Secretos en `.env`; nunca se registran tokens ni contenido.
- HMAC-Streebog + timestamp + nonce para el sistema externo.
- Telegram es un tercero aceptado solo para notificación/deep link.
- La IP de `X-Forwarded-For`/`X-Real-IP` solo es confiable si el reverse proxy
  las sobrescribe; el `ip` del body es aportado por el cliente y no se verifica.
- El estado de llamada es efímero: no sobrevive a un reinicio del servidor.

## Tablas/SPs
- `opr_llamadas_integracion_codigos`
- `opr_llamadas_integracion_vinculos`
- `opr_llamadas_integracion_externa`
- `procLlamadasIntegracionVinculosProc`, `procLlamadasIntegracionVinculosCons`
- `procLlamadasIntegracionExternaProc`, `procLlamadasIntegracionExternaCons`

Ver `db_sodastream/video_llamadas_integraciones.sql`.

Implementación del cliente (Android/Qt): ver `docs/telegram_client.md`.
