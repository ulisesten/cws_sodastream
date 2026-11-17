# E2EE de llamadas — pendiente (cliente + API)

Actualizado: 2026-11-16. Complementa `docs/calls_e2ee.md` (contrato) y
`docs/calls_status.md` (estado del servidor).

## Estado verificado

- Cliente Android: audio y vídeo cifrados con **FrameCryptor-GOST**
  (Kuznyechik-CTR-ACPKM + CMAC) en `libwebrtc-gost.aar`.
- Verificado en llamada real: `e2ee applied (FrameCryptor GOST)` en ambos
  extremos, `framesDecoded > 0`, misma clave derivada por VKO/HKDF.
- Test de loopback instrumentado del `FrameCryptor` GOST: PASS en Pixel 7 Pro
  y Pixel 3 XL.
- Código de seguridad (Streebog-256 de la clave) visible en la pantalla de
  llamada para comparación verbal.

## P0 — seguridad (bloquea producción)

### 1. Autenticación de la clave efímera (MITM del signaling)

> Servidor: ya existe `POST/GET /calls/identity` para publicar/consultar claves
> públicas por dispositivo (`video_llamadas_identidad.sql`). Falta que el
> cliente firme el SDP con la identidad y verifique TOFU/safety number, o el
> flujo X3DH con prekeys de un solo uso.

El servidor (o un MITM de TLS) puede sustituir el atributo
`a=x-gost-e2ee:v1:<pub>` y quedar en medio. Hace falta una de estas vías:

- **X3DH-GOST**: prekeys firmadas + prekeys de un solo uso.
  - `POST /api/v1/calls/prekeys` (subir bundle firmado con la clave de
    identidad del usuario).
  - `GET  /api/v1/calls/prekeys/:usu_id` (obtener bundle).
  - `DELETE /api/v1/calls/prekeys/:usu_id/:prekey_id` (consumir una
    one-time prekey de forma atómica).
- **o** firma del atributo SDP con la clave de identidad
  (`a=x-gost-identity:v1:<sig>`), verificada contra la clave pública del
  contacto guardada por TOFU.
- Safety number/QR derivado de ambas claves de identidad.

La API debe almacenar claves públicas de identidad y prekeys, nunca claves
privadas. Sin esto, el E2EE solo protege frente a TURN/observadores de red,
no frente al propio servidor.

### 2. Confirmación de clave (key confirmation)

Hoy un desacuerdo de claves se manifiesta como “no hay vídeo”. Implementar:

- **Cliente (preferido)**: DataChannel `e2ee-confirm` con
  `HMAC-Streebog-256(key, "confirm-v1" | call_id | pubA | pubB)`; ambos
  extremos comparan y solo entonces habilitan los tracks.
- **Servidor (alternativa)**: eventos `call.e2ee_confirm` con un tag opaco de
  32 B; el relay lo entrega al otro extremo y **rechaza** la llamada si no
  coincide en 10 s (`E2EE_KEY_MISMATCH`). El servidor no puede calcularlo.

### 3. Ratchet

Rotación de clave por frame/intervalo con Streebog y ventana de recepción.
Sin ratchet no hay post-compromise security. No requiere cambios de API salvo
negociación de versión.

### 4. RNG e IV en el fork de WebRTC

`makeIv` usa `srand(time(NULL))`/`rand()` para el contador inicial. Cambiar a
`RAND_bytes` (BoringSSL) o `getrandom` y garantizar un contador monotónico por
SSRC y por clave. Probar no-reutilización tras reinicio del proceso.

### 5. Derivación GOST-first en el fork

El `FrameCryptor` deriva la clave de frame con **PBKDF2-SHA256** (100k
iteraciones) a partir del material VKO/HKDF. Migrar a
`PBKDF2/HKDF-Streebog-256` o documentar la excepción y su justificación.

### 6. Borrado de material

Zeroizar el key ring y los `ParticipantKeyHandler` nativos al colgar; no solo
los `ByteArray` de Java. Cubrir rutas de error y `dispose()` fuera de orden.

### 7. Rekey y reconexión

`WebRtcEngine.setE2eeKey` ignora una segunda clave distinta (falla cerrado).
Definir si se soporta rekey (nueva llamada, ICE restart) o se rechaza con
`E2EE_REKEY_UNSUPPORTED`.

## P1 — protocolo y robustez

### 8. Validación estricta del SDP

- Exactamente un `a=x-gost-e2ee:v1:<base64>` por SDP.
- `v1` en oferta y respuesta; versión desconocida → error explícito.
- Base64 canónico, 64 B, punto en curva y subgrupo (ya se valida en VKO).
- Límite de tamaño de SDP en la API (p. ej. 256 KB) y rechazo de atributos
  duplicados.

### 9. Matriz de codecs

Verificar H.264, VP9 y AV1 (hoy VP8) y el *RBSP escaping* de H.264 con el tag
GOST y el IV de 16 B. Añadir tests de loopback por codec.

### 10. Keyframe al instalar el decryptor

Pedir PLI/FIR al activar el `FrameCryptor` del receptor y eliminar los
`postDelayed(300)` heurísticos; engancharse a eventos de transceiver/track.

### 11. Indicador y telemetría E2EE

El cliente ya muestra `E2EE GOST · <código>`. Falta:

- Estado “E2EE verificado” tras confirmación de clave.
- Contador de fallos de descifrado (sin claves ni SDP) para diagnóstico.
- Error visible si el cryptor queda deshabilitado.

### 12. Pruebas de red y ciclo de vida

- Wi-Fi ↔ datos móviles, TURN forzado, pérdida/latencia, llamadas largas.
- Cambio de red, pantalla bloqueada, app en segundo plano/FGS.
- Llamada entrante con la app cerrada y carrera SDP/ICE.
- Colgar sin WS/media residual (verificado en audio; repetir con el fork).

## P1 — API/señalización

### 13. Capacidad y versión E2EE en la invitación

> Servidor: implementado. `notify`/`accept` exigen `e2ee` con
> `CALL_E2EE_REQUIRED=true` y lo incluyen en `call.incoming`/`call.accepted`.
> El cliente debe enviarlo y hacer fail-closed.

Añadir a `call.incoming`/`notify`:

```json
"e2ee": { "v": 1, "algs": ["gost"], "confirm": true }
```

El cliente debe rechazar la llamada si el par no anuncia `gost` (fail-closed
antes de crear el PeerConnection). La API puede rechazar `notify` sin
capacidad E2EE si la política lo exige.

### 14. `call_id` impredecible y de un solo uso

> Servidor: implementado. `CALL_ID_SERVER=true` genera 32 chars, controla
> colisión y lo devuelve en la respuesta; el cliente debe usar ese valor.

- 128 bits aleatorios (UUIDv4/ULID) generados por el servidor.
- Rechazar reutilización con otros participantes (hoy el upsert puede
  sobrescribir estado).
- TTL y limpieza; no aceptar `call_id` de otro usuario.

### 15. Relay de SDP/ICE endurecido

> Servidor: tamaño, tipo, participación y estado `accepted` validados; falta
> rate limit por llamada/usuario y no loguear SDP (hoy no se loguea).

- `POST /calls/:id/sdp`: validar tamaño, `type`, autoría y que el `call_id`
  pertenece a la llamada; no loguear el SDP completo.
- `call.sdp`/`call.ice`: entregar solo al par autorizado (no broadcast).
- Rate limit por llamada y por usuario.
- Rechazar SDP tras `ended`.

### 16. Credenciales TURN por llamada

Ya existen credenciales temporales (coturn REST). Revisar:

- TTL corto (≤ 10 min) y renovación.
- `turns:5349` sigue cerrado; publicar 5349/TCP o documentar que solo hay
  `turn:3478`.
- Cuotas por usuario y por IP.

### 17. Identidad y multi-dispositivo

> Servidor: directorio de identidad pública implementado. Falta vincular la
> clave al `usu_id`/dispositivo en el flujo cliente y el multi-dispositivo.

- Vincular la clave de identidad GOST al `usu_id` y a sus dispositivos.
- Al aceptar/rechazar, avisar a los demás dispositivos del receptor.
- CDR sin material de claves; no guardar SDP ni claves efímeras.

### 18. Códigos de error

Definir y documentar: `E2EE_UNSUPPORTED`, `E2EE_VERSION_UNSUPPORTED`,
`E2EE_KEY_MISMATCH`, `E2EE_CONFIRM_TIMEOUT`, `E2EE_REKEY_UNSUPPORTED`,
`E2EE_DECRYPT_FAILED`.

## P2 — distribución y auditoría

### 19. AAR multi-ABI y reproducible

- Compilar `arm64-v8a`, `armeabi-v7a`, `x86_64` (hoy solo arm64).
- Firmar el AAR y publicarlo en un Maven interno; no commitear 12 MB en
  `app/libs`.
- Pin del commit del fork (`webrtc-sdk/webrtc` `m125_release` + parches) y CI
  que compile el AAR, la app y los tests de loopback.

### 20. 16 KB pages

`libgostsframe.so` y `libjingle_peerconnection_so.so` ya están alineados a
16 KB. Quedan VLC y otras libs del APK; actualizar NDK (r27+) y dependencias
para Play en dispositivos 16 KB.

### 21. Auditoría externa

- Construcción AEAD (Kuznyechik-CTR-ACPKM + CMAC, encrypt-then-MAC).
- VKO/curva TC26-256-A y HKDF-Streebog.
- Formato de frame, AAD y manejo de H.264.
- Fork de WebRTC (IV, derivación, borrado).
- Modelo de amenazas: signaling, TURN, metadata, DNS.

### 22. Fuera de alcance 1:1

- Grupo/SFU con SFrame-GOST o gateway que no descifre.
- Multi-dispositivo simultáneo.
- Interop con otros clientes (versionado del formato de frame).
