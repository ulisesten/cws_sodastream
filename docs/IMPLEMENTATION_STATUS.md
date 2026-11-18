# Estado de implementación — fuente única

Actualizado: 2026-11-18. Usa los estados definidos en `AGENTS.md`.
No sustituye a los documentos de detalle; los enlaza.

## Leyenda

`[IMPLEMENTADO]` · `[PARCIAL]` · `[CLIENTE]` · `[PENDIENTE]` · `[BLOQUEADO]` · `[OPS]`

## Llamadas / E2EE

| Área | Estado | Lado | Detalle |
|---|---|---|---|
| Señalización 1:1 (`/calls/:id`, SDP/ICE relay, TURN) | [IMPLEMENTADO] | Servidor | `calls_routes.c`, `turn_deploy.md` |
| Estado de llamada / notify / accept / reject / end / pending | [IMPLEMENTADO] | Servidor | en memoria; se pierde al reiniciar |
| `call_id` generado por servidor + capacidad E2EE | [IMPLEMENTADO] | Servidor | `CALL_ID_SERVER`, `CALL_E2EE_REQUIRED` |
| Directorio `/calls/identity` | [IMPLEMENTADO] | Servidor | claves públicas; sin validación de curva aún (M8) |
| Firma del SDP / TOFU / safety number | [PARCIAL] | Servidor + cliente | primitivas GOST y directorio sí; firma/verificación SDP no (C1) |
| FrameCryptor-GOST audio/vídeo | [IMPLEMENTADO] | Cliente | reportado/verificado en docs del cliente; falta auditoría |
| Key confirmation / ratchet | [PENDIENTE] | Cliente | A6 |
| CDR persistente / historial / missed | [PENDIENTE] | Servidor | `calls_status.md` |
| Multi-dispositivo de llamadas (cancelar otros) | [PENDIENTE] | Servidor + cliente | `calls_status.md` |
| Grupo / walkie / Janus gateway | [PENDIENTE] | Servidor | fase posterior |

## Push / notificaciones

| Área | Estado | Lado | Detalle |
|---|---|---|---|
| WSS `call.incoming` / `dm` | [IMPLEMENTADO] | Servidor | app abierta/conectada |
| Telegram link + deep link | [IMPLEMENTADO] | Servidor | `telegram_bot.md` |
| Multi-dispositivo (`/users/device`) | [IMPLEMENTADO] | Servidor | `usuarios_devices.sql` |
| FCM emisor (OAuth2/HTTP v1), outbox, call/message, ack | [PARCIAL] | Servidor | `9a3a7f4` + ack; falta service account, preferencias, rate limit, E2E. Si `enqueued=0`, verificar que el SP desplegado tenga `@PUSH_TOKENS=4` (`db_sodastream/usuarios_devices.sql`). |
| FCM cliente (Firebase/PushService + delivery.ack) | [IMPLEMENTADO] | Cliente | `RealtimeClient` ackea `dm`/`call.incoming` con `event_id`+`device_id` al recibir el WSS; Firebase/PushService ya está. Falta instalar/probar el APK. |
| Tipos silenciosos/summary/read/deleted/missed | [PENDIENTE] | Servidor + cliente | `fcm_notifications.md` |
| Cifrado de `usd_push_token` en reposo | [PENDIENTE] | Servidor | M5 |
| Rate limiting global / cuotas TURN | [PENDIENTE] | Servidor | A1 |
| APNs / WebPush | [PENDIENTE] | Servidor | fuera de alcance actual |

## Seguridad / operación

| Área | Estado | Lado | Detalle |
|---|---|---|---|
| RNG/IV del FrameCryptor | [IMPLEMENTADO] | Cliente/fork | C3, commit `027d75b`; falta test dispositivo |
| Firma/identidad SDP | [PARCIAL] | Servidor + cliente | ver arriba |
| Hashes legacy de contraseña | [PENDIENTE] | Servidor | A5 |
| Pinning / cleartext / backup / release | [PENDIENTE] | Cliente | C2/A3/A4/A7 |
| CORS / DB TLS / `.env` | [PENDIENTE] | Servidor/ops | M9/A2 |
| Auditoría criptográfica externa | [PENDIENTE] | Ambos | C1/A6/A8/P2 |
| SAST/DAST/CI y tests de autorización | [PENDIENTE] | Servidor/CI | B6 |
