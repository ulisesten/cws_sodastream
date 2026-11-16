# Estado de llamadas — servidor

Actualizado: 2026-11-16. Complementa `docs/calls_proposal.md` y
`docs/telegram_client.md`.

## Implementado

- Vinculación Telegram: código de un solo uso, `chat_id ↔ usu_id`.
- Aviso de llamada: WSS `call.incoming`; Telegram con `cws://call/<id>` si no
  hay socket.
- Estado de llamada en memoria: `ringing/accepted/ended/rejected`, TTL 90 s /
  6 h.
- `notify`, `accept`, `reject`, `end`, `pending` bajo
  `/api/v1/integrations/telegram`.
- IP/teléfono del solicitante y receptor; mapeo persistente
  (`opr_llamadas_integracion_*`).
- API externa HMAC-Streebog (`/integrations/external/link|notify`).
- Señalización 1:1: `GET /calls/:id`, `GET /calls/:id/ice`,
  `POST /calls/:id/sdp`, `POST /calls/:id/ice`.
- Relay WSS `call.sdp` / `call.ice`.
- Credenciales TURN temporales (coturn REST, HMAC-SHA1); STUN/TURN.
- Lib GOST: Kuznyechik, CTR-ACPKM, CMAC, HKDF, AEAD, SFrame-GOST.

## Pendiente del lado servidor

### 1:1 (prioridad)

1. **CDR persistente**: tablas `opr_llamadas` /
   `opr_llamadas_participantes`, SPs, historial y missed calls. Hoy el estado
   se pierde al reiniciar.
2. **Aliases `/api/v1/calls`**: `POST /calls`, `/accept`, `/reject`, `/end`
   (hoy viven bajo `/integrations/telegram`).
3. **Multi-dispositivo**: al aceptar/rechazar, avisar a los demás dispositivos
   del receptor (`call.accepted`/`cancelled`) para que dejen de sonar.
   Añadir `device_id` si se quiere dirigir por dispositivo.
4. **Timeout/missed**: al expirar el ringing, emitir `call.timeout`/`missed`,
   cerrar CDR y decidir notificación.
5. **Idempotencia y colisión de `call_id`**: rechazar un `call_id` existente
   con otros participantes; el `upsert` actual puede sobrescribir estado.
6. **Autorización/abuso**: contactos/bloqueo/quién puede llamar; rate limit de
   `notify`/`accept`; límite de llamadas concurrentes; cuotas TURN.
7. **Estado compartido/HA**: el estado está en memoria por proceso. Con varias
   instancias hace falta sticky/shard o store compartido (DB/Redis self-hosted).
8. **Métricas/observabilidad**: llamadas activas, setup success, relay TURN,
   timeouts, errores; logs sin SDP/tokens/teléfonos.
9. **Tests automatizados**: E2E de create/accept/SDP/ICE/TURN y multi-dispositivo.

### Grupo / walkie (secundario)

10. **Gateway C ↔ Janus**: API/eventos, tokens Janus, backpressure, HA;
    Janus no expuesto.
11. **VideoRoom**: salas, participantes, simulcast (no SVC), límites.
12. **Sender keys/epoch**: el servidor solo señaliza; la gestión de claves es
    del cliente.
13. **Walkie**: floor control, lease, cola/prioridad, half-duplex, pausa de
    suscripciones; Janus.
14. **CDR de grupo** y calidad agregada.

### Integración / operación

15. **Resolve/webhook externo** (si el sistema externo lo necesita): consultar
    teléfono/IP → `usu_id_public` y recibir eventos; hoy solo hay link/notify.
16. **Retención/privacidad**: purga de IP/teléfono, CDR y metadatos.
17. **TURN**: cuotas, rotación de secreto, HA, métricas y certificados.
18. **FCM/APNs**: aprobado para después; hoy WSS + Telegram.
19. **GOST TLS/pinning** en el borde/cliente: objetivo; no es código de llamadas.

## Nota

El media WebRTC (RTCPeerConnection, candidatos, SFrame-GOST) es del cliente. El
servidor ya releva SDP/ICE y entrega TURN; no abre RTP ni descifra.
