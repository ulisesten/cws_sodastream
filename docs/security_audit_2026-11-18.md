# Análisis de seguridad — app Android + API de llamadas/chat

Fecha: 2026-11-18. Alcance: revisión estática de `mobile/` (Kotlin/JNI),
`cws_sodastream` (C/ODBC/WS), fork `webrtc-gost-build` y configuración de
despliegue. **No es un pentest dinámico**; no se probaron ataques en red ni
explotación. Las rutas de evidencia son relativas a cada repo.

## Resumen ejecutivo

| Severidad | Nº | Temas principales |
|---|---:|---|
| Crítica | 2 abiertas (+1 corregida) | Identidad E2EE y cleartext; RNG/IV corregido en el fork (C3) |
| Alta | 8 | Rate limiting, secretos `.env`, backup, pinning, hashes legacy, release/debug, confirmación/ratchet, zeroización/derivación |
| Media | 9 | Notificaciones, sticker paths, enumeración/spam, deep link, tokens push en BD, AAR/fork, FCM servidor, TURN/STUN, CORS |
| Baja | 6 | Logs, handshake WS, FLAG_SECURE, API key, 16 KB, CI |

Controles positivos: tokens cifrados con Android Keystore (AES-GCM),
PBKDF2-HMAC-Streebog + salt + pepper para hashes nuevos, comparación en tiempo
constante, refresh con validación de `usu_salt` (el logout global rota el
salt), autorización de participantes en SDP/ICE, validación de
`call_id`/`public_id`/sticker id, límites de tamaño, E2EE fail-closed,
`FrameCryptor` descarta frames sin clave y TLS estándar en API/WSS.

---

## Actualización de remediación (2026-11-18)

- **C3 (RNG/IV) corregido** en el fork `webrtc-gost-build` commit
  `027d75b`: el nonce inicial usa `RAND_bytes` (con fallback
  `std::random_device`); se eliminó `srand(time(NULL))`/`rand()`. El AAR
  `libwebrtc-gost.aar` se regeneró (sha256 `4375f2486aac8d94d5d6…`) y se
  copió a `app/libs/`; la app compila. **Pendiente:** ejecutar el test de
  loopback en dispositivo cuando se reconecten (ahora están desconectados)
  antes de distribuir.
- El resto de hallazgos sigue abierto salvo que se indique lo contrario.

## Estado de remediación y responsabilidad

> Revisado contra el código de este workspace. La API está en
> `cws_sodastream` y el cliente Android en
> `/home/ulises/AndroidStudioProjects/mobile` (repo aparte, pero accesible en
> el mismo workspace). Los estados “cliente” se comprobaron en el árbol actual;
> no hay un commit del cliente que fijar todavía.

| ID | Estado | Lado | Qué falta |
|---|---|---|---|
| C1 | **Parcial** | Servidor + cliente | Servidor: primitivas `gost_signature_*` y directorio `/calls/identity`; no hay firma ni verificación de `a=x-gost-identity` en `calls_sdp_handler`. Cliente: firmar el SDP, verificar contra `/calls/identity`, TOFU/safety number. Servidor: exigir/verificar si se quiere fail-closed. |
| C2 | Pendiente | Cliente | `usesCleartextTraffic=false`, `cleartextTrafficPermitted=false`, solo `https`/`wss`, pinning. |
| C3 | **Implementado (fork)** | Cliente | `RAND_bytes` en `027d75b`; AAR regenerado. Falta test de loopback en dispositivo y CI de no-reutilización. |
| A1 | Pendiente | Servidor | Rate limit por IP/usuario/`call_id` en auth, notify, accept, messages, device, SDP/ICE; 429/Retry-After; cuotas TURN; límite de dispositivos. |
| A2 | Pendiente | Ops/servidor | `.env` 600, propietario dedicado, `UMask=0077`, rotar secretos, no commitear. |
| A3 | Pendiente | Cliente | `allowBackup=false` o excluir prefs/stickers; cifrar chat/calls. |
| A4 | Pendiente | Cliente | Pinning con backup pin en API y WS; exigir `Sec-WebSocket-Accept`. |
| A5 | Pendiente | Servidor | Rehash de hashes legacy al login; subir iteraciones; actualizar fila. |
| A6 | Pendiente | Cliente | Key confirmation por DataChannel/API; ratchet con ventana. |
| A7 | Pendiente | Cliente | Release firmada, R8, `debuggable=false`, `FLAG_SECURE`. |
| A8 | Pendiente | Cliente/fork | PBKDF2/HKDF-Streebog-256; zeroización de `KeySet`; tests. |
| M1 | Pendiente | Servidor + cliente | Contactos/opt-in, bloqueo/reporte, rate limit, no listar toda la base. |
| M2 | Pendiente | Cliente | Notificaciones de mensajes `VISIBILITY_PRIVATE` + public version. |
| M3 | Pendiente | Cliente | Validar `^[0-9a-f]{64}$` y canonicalizar stickers. |
| M4 | Pendiente | Cliente | Validar `call_id`/`chat_id` del deep link antes de usarlo. |
| M5 | Pendiente | Servidor + cliente | Cifrar `usd_push_token` en reposo, rotación, retención, límite por usuario. |
| M6 | Parcial | Servidor | Emisor FCM/outbox implementado; falta service account, preferencias, rate limit/métricas y pruebas reales. |
| M7 | Pendiente | Cliente/CI | AAR en Maven interno, pin de commit/hash, build reproducible, multi-ABI. |
| M8 | Pendiente | Servidor + cliente | Validar base64/punto TC26-256-A en `/calls/identity`; TOFU. |
| M9 | Pendiente | Servidor/ops | CORS allowlist (o desactivar en móvil), `TrustServerCertificate=false` + CA. |
| B1 | Pendiente | Servidor + cliente | Logs de release sin tokens/huellas; no ampliar payloads. |
| B2 | Pendiente | Cliente | Exigir `Sec-WebSocket-Accept` en el handshake. |
| B3 | Pendiente | Cliente | `FLAG_SECURE` en chat/llamada. |
| B4 | Pendiente | Cliente/ops | Restringir API key por package/SHA-1; `.gitignore` para `google-services.json`. |
| B5 | Pendiente | Cliente/CI | Actualizar libs a 16 KB. |
| B6 | Pendiente | Servidor/CI | SAST/DAST, dependency scanning, tests de autorización/rate limit. |

### Firmas: qué está y qué no

- **Implementado:** primitivas GOST R 34.10-2012 en `c_gost_encryption`
  (`gost_signature_sign*`/`verify*`), directorio de claves públicas
  `/api/v1/calls/identity`, capacidad E2EE y `call_id` de servidor.
- **No implementado en el flujo de llamada:** el SDP solo lleva
  `a=x-gost-e2ee:v1:<pub>`; no hay `a=x-gost-identity:v1:<sig>` ni
  verificación en `calls_sdp_handler`; el cliente no usa el directorio para
  TOFU/safety number.
- **Cliente:** debe firmar el payload canónico (`v1|call_id|type|pub`),
  publicar la firma en el SDP y verificar la del par con la clave de
  `/calls/identity/:public`.

### Falta de este lado (servidor)

1. Rate limiting/429/cuotas TURN y límite de dispositivos (A1).
2. Verificación/exigencia de firma SDP e identidad (C1) y validación de clave
   en `/calls/identity` (M8); depende del formato que implemente el cliente.
3. Rehash de hashes legacy en login (A5).
4. Contactos/bloqueo y no enumeración de usuarios (M1).
5. Cifrado/retención de `usd_push_token` (M5).
6. FCM servidor (M6): configuración/service account, preferencias y pruebas reales; el emisor/outbox ya existe.
7. CORS/DB TLS (M9), `.env` (A2), logs (B1), CI/SAST (B6).
8. CDR/multi-dispositivo de llamadas (fuera de este audit, pendiente ya
   documentado en `docs/calls_status.md`).

## Críticos

### C1. E2EE sin autenticación de identidad (MITM del signaling)

- **Evidencia:** `CallViewModel.addE2eeAttribute()` publica
  `a=x-gost-e2ee:v1:<pub>` sin firma; el servidor releva SDP sin verificarlo
  (`calls_sdp_handler`). El directorio `/calls/identity` existe pero el cliente
  no lo usa. El “código de seguridad” es manual y no vinculante.
- **Impacto:** el servidor de señalización (o un MITM de TLS/CA) puede
  sustituir la clave efímera y quedar en medio; el E2EE protege del TURN, no
  del propio backend.
- **Solución:** clave de identidad GOST persistente por dispositivo; firmar el
  SDP (`a=x-gost-identity:v1:<sig>`) y verificar con la clave publicada en
  `/calls/identity/:public`; TOFU/pinning + safety number/QR. Alternativa:
  X3DH-GOST con prekeys firmadas. Sin esto, no considerar el E2EE “seguro
  frente al servidor”.

### C2. Cleartext permitido en toda la app

- **Evidencia:** `app/src/main/AndroidManifest.xml`
  `android:usesCleartextTraffic="true"` y
  `res/xml/network_security_config.xml` con
  `<base-config cleartextTrafficPermitted="true" />`.
- **Impacto:** cualquier URL HTTP (presente o futura) puede ir en claro;
  facilita downgrade/MITM y filtraciones de tokens. Play/Android 9+ lo
  penalizan.
- **Solución:** `usesCleartextTraffic="false"`; base-config
  `cleartextTrafficPermitted="false"`; permitir solo `https`/`wss`; añadir
  pinning (con backup pin) para `cws.sodastream.fun`.

### C3. RNG/IV débil en el fork del FrameCryptor — **CORREGIDO**

- **Evidencia:** `webrtc-gost-build/src/api/crypto/frame_crypto_transformer.cc`
  `makeIv()` usaba `srand((unsigned)time(NULL)); rand() * 0xFFFF` para el
  contador inicial.
- **Impacto:** IV predecible/repetible entre procesos con la misma semilla;
  con la misma clave compartida podía degradar la garantía del AEAD
  (Kuznyechik-CTR-ACPKM + CMAC).
- **Solución aplicada:** commit `027d75b`; `RAND_bytes` (BoringSSL) con
  fallback `std::random_device`; se mantiene el contador monotónico de 32 bits
  por SSRC. El AAR se regeneró y se copió a `app/libs/` (sha256
  `4375f2486aac8d94d5d6…`); la app compila. Pendiente: test de loopback en
  dispositivo (sin dispositivos `adb` ahora) y test de no-reutilización en CI.

---

## Altos

### A1. Sin rate limiting en autenticación, llamadas, chat y dispositivos

- **Evidencia:** no hay respuestas 429 ni contadores en `src/api/v1` (solo
  límites de paginación). `/notify`, `/accept`, `/messages`, `/users/device`,
  `/calls/:id/sdp|ice`, `/setup` aceptan peticiones ilimitadas.
- **Impacto:** fuerza bruta de contraseñas, spam de llamadas/mensajes,
  creación masiva de dispositivos, DoS de SDP/ICE/WS y abuso de TURN.
- **Solución:** límites por IP + usuario + `call_id`; backoff y bloqueo
  temporal tras N fallos; 429 con `Retry-After`; cuotas TURN; límite de
  dispositivos por usuario; rate limit en el WS.

### A2. Secretos en `.env` con permisos 0644

- **Evidencia:** `ls -l .env` → `-rw-r--r--`; contiene `SECRET_KEY`,
  `DB_PASSWORD`, `TURN_SECRET`, token de Telegram y credenciales de correo.
- **Impacto:** cualquier usuario local del servidor puede leerlos y firmar
  JWTs, acceder a la BD, generar credenciales TURN o usar el bot.
- **Solución:** `chmod 600`, propietario dedicado, systemd `EnvironmentFile`
  con `UMask=0077`, rotar todos los secretos, valorar Vault/KMS. No commitear
  `.env` (hoy no está trackeado; mantenerlo así).

### A3. Backup Android habilitado con datos de chat/llamadas en claro

- **Evidencia:** `allowBackup="true"` con `backup_rules.xml` y
  `data_extraction_rules.xml` vacíos. `messaging_conversations.xml`,
  `calls.xml`, `device_identity.xml` (device_id + push token) y los stickers
  no están cifrados; solo `auth_session` usa Keystore.
- **Impacto:** un backup en la nube/extracción puede llevarse contactos,
  últimos mensajes, historial de llamadas y tokens push (no los access
  tokens, que están cifrados y ligados al Keystore).
- **Solución:** `allowBackup="false"` o excluir `sharedpref` sensibles y
  `files/sticker_packs`; cifrar datos de chat/calls con Keystore; probar
  `adb backup`/restore.

### A4. Sin certificate pinning y validación WS opcional

- **Evidencia:** `network_security_config` no tiene `<pin-set>`; Volley/WS usan
  CA del sistema. En `GostWebSocketClient`, el `Sec-WebSocket-Accept` solo se
  compara si el header existe (`receivedAccept != null`).
- **Impacto:** una CA comprometida/MITM puede interceptar tokens, SDP y
  metadata (el media sigue E2EE, pero la señalización no está autenticada,
  ver C1).
- **Solución:** pinning con backup pin en API y `SSLSocket` del WS; exigir el
  header `Sec-WebSocket-Accept` (fallar si falta); validar `wss`/`https`
  estrictamente.

### A5. Hashes de contraseña legacy aceptados y no rehasheados

- **Evidencia:** `core/password.c` verifica PBKDF2-HMAC-Streebog, pero acepta
  el formato legacy `Streebog-256` sin salt; `password_needs_rehash()` está
  declarado y **no se usa** en `authorization_signin`.
- **Impacto:** si la BD se filtra, los hashes legacy son rápidos de romper
  (GPU/ASIC), sobre todo cuentas antiguas.
- **Solución:** al autenticar con `PASSWORD_OK_LEGACY`, rehashear a PBKDF2 y
  actualizar la fila; forzar cambio si el hash es legacy; subir iteraciones a
  un valor medido (p. ej. 200k-600k) y documentar.

### A6. Sin confirmación de clave ni ratchet

- **Evidencia:** `CallViewModel` muestra un código Streebog manual; no hay
  MAC cruzado por DataChannel/API; el FrameCryptor usa `ratchetWindowSize=0`.
- **Impacto:** un desacuerdo de claves se detecta tarde (o nunca si el usuario
  no compara); sin post-compromise security.
- **Solución:** confirmación automática
  `HMAC-Streebog(key, "confirm-v1"|call_id|pubA|pubB)` por DataChannel o API;
  habilitar/implementar ratchet por frame/intervalo con ventana y recuperación.

### A7. Release sin hardening y builds debug distribuidas

- **Evidencia:** `app/build.gradle.kts` `isMinifyEnabled=false`, sin
  `signingConfig` de release; se instalan APKs debug (`debuggable=true`).
  No hay `FLAG_SECURE` en chat/llamada.
- **Impacto:** ingeniería inversa trivial, `run-as`/debug en builds
  distribuidas, capturas/recents con mensajes y vídeo.
- **Solución:** build release firmada, R8/obfuscación, `debuggable=false`,
  keystore de release fuera del repo; `FLAG_SECURE` en chat/call (o
  `setRecentsScreenshotEnabled(false)` en API 33+); revisar logs en release.

### A8. Derivación de claves del fork no GOST y zeroización incompleta

- **Evidencia:** `frame_crypto_transformer.h` usa
  `DerivePBKDF2KeyFromRawKey` (PBKDF2-HMAC-SHA256, 100k) sobre el material
  VKO/HKDF; el key ring nativo se libera al `dispose`, pero no hay borrado
  explícito de cada `KeySet` en todas las rutas.
- **Impacto:** rompe el requisito “GOST-first”; el material puede quedar en
  memoria más de lo necesario.
- **Solución:** PBKDF2/HKDF-Streebog-256 en el fork; `gost_secure_zero` de
  `KeySet`/material al rotar/cerrar; tests de zeroización; documentar la
  decisión si se mantiene SHA-256.

---

## Medios

### M1. Enumeración de usuarios y spam sin bloqueo

- **Evidencia:** `GET /messaging/users` devuelve todos los usuarios con
  `usu_id_public`/nombre a cualquier autenticado; `/notify` y `/messages`
  aceptan cualquier `public_id`.
- **Impacto:** enumeración de la base de usuarios, spam de llamadas/mensajes,
  acoso; sin lista de bloqueo/reportes.
- **Solución:** contactos/opt-in, bloqueo/reporte, rate limit, no exponer
  todos los usuarios por defecto (búsqueda por ID), captcha para web.

### M2. Privacidad de notificaciones de mensajes (verificar)

- **Evidencia:** `CallNotifications.showMessage` no fija explícitamente
  `setVisibility`/`setPublicVersion` ni `lockscreenVisibility` en el canal
  `messages`. El valor por defecto de Android es `VISIBILITY_PRIVATE`, pero
  conviene no depender de él. Las llamadas usan `VISIBILITY_PUBLIC` a
  propósito.
- **Impacto:** si el usuario permite contenido en la pantalla de bloqueo o el
  canal se creó con otra visibilidad, el texto del mensaje podría mostrarse.
- **Solución:** fijar `VISIBILITY_PRIVATE` + `setPublicVersion` (“Nuevo
  mensaje”) en mensajes y `lockscreenVisibility=PRIVATE` en el canal; dejar
  las llamadas públicas.

### M3. Defensa en profundidad en rutas de stickers

- **Evidencia:** `StickerStore.fileFor/saveReceived/delete` construyen
  `File(dir, "$id.webp")` sin validar el id en el cliente. El servidor valida
  64 hex (`valid_sticker_id`), pero el cliente depende de ello.
- **Impacto:** si un id malicioso llegara por otra vía (BD/API comprometida,
  migración), path traversal de lectura/escritura/borrado limitado a `.webp`.
- **Solución:** validar `^[0-9a-fA-F]{64}$` en `StickerStore` y al parsear
  `sticker_id`; canonicalizar y comprobar que el `File` está dentro del root.

### M4. Deep link de llamada sin validación de formato

- **Evidencia:** `MainActivity.extractCallId` valida scheme/host y toma el
  primer segmento, sin longitud/charset. El servidor valida 6-64
  alfanumérico/`_-`.
- **Impacto:** ids enormes/raros provocan peticiones y estados de error; no
  hay creación de llamadas, pero sí ruido/DoS menor.
- **Solución:** validar `^[A-Za-z0-9_-]{6,64}$` antes de `recoverCall`; ignorar
  el resto; validar también `open_chat` (12 chars).

### M5. Tokens push y metadatos en la BD

- **Evidencia:** `opr_usuarios_devices.usd_push_token` en claro; el GET de
  dispositivos no lo devuelve (bien), pero la BD sí lo almacena.
- **Impacto:** acceso a la BD → envío de push a los dispositivos; los tokens
  no son claves de datos, pero permiten spam/phishing.
- **Solución:** cifrar el token en reposo (KMS/clave de app), rotación,
  `DELETE` al logout (ya implementado), limitar dispositivos por usuario,
  restringir la cuenta de servicio FCM.

### M6. FCM del servidor no implementado

- **Evidencia:** no hay módulo FCM en `cws_sodastream`; los endpoints
  `/users/device` guardan token, pero nada envía. El `google-services.json`
  es configuración **cliente**, no el service account.
- **Impacto:** sin aviso cuando el WS está caído/app matada (disponibilidad,
  no confidencialidad). Al implementarlo, un manejo descuidado del service
  account sería crítico.
- **Solución:** service account fuera del repo (`FCM_CREDENTIALS`), OAuth2
  HTTP v1, data-only para llamadas, multi-dispositivo, borrar tokens
  `UNREGISTERED`, limitar API key por package/SHA-1; no incluir SDP/claves.

### M7. Cadena de suministro del AAR/fork

- **Evidencia:** `app/libs/libwebrtc-gost.aar` (12 MB) y el fork no están
  trackeados/pinneados con hash; no hay CI que reconstruya y compare. Solo
  arm64.
- **Impacto:** un AAR alterado o un fork no reproducible puede introducir
  código malicioso en el media.
- **Solución:** publicar el AAR en Maven interno firmado, pin de commit SHA,
  build reproducible, hash verificado en Gradle, CI con tests de loopback;
  multi-ABI y 16 KB.

### M8. Endpoint de identidad sin validación de clave

- **Evidencia:** `POST /calls/identity` acepta `identity_key` de 1-500 chars
  sin validar base64/curva; `GET` devuelve las identidades a cualquier
  autenticado.
- **Impacto:** datos basura/dirigidos; cuando el cliente firme SDP, una clave
  inválida puede romper la verificación o facilitar TOFU envenenado.
- **Solución:** validar base64 y punto en TC26-256-A (64 B), una clave por
  dispositivo, auditoría de cambios, rate limit; TOFU en el cliente.

### M9. CORS abierto y confianza del certificado de BD

- **Evidencia:** el middleware CORS por defecto responde
  `Access-Control-Allow-Origin: *` si está montado en el router móvil
  (verificar); la conexión ODBC usa `TrustServerCertificate` según
  `.env`/config.
- **Impacto:** una web maliciosa puede llamar a la API si obtiene un bearer
  (no lee cookies); el certificado de BD no se valida (MITM interno).
- **Solución:** allowlist de orígenes (o deshabilitar CORS en móvil),
  `TrustServerCertificate=false` con CA interna, red privada/segmentada.

---

## Bajos

- **B1. Logs:** se registra el prefijo del token FCM y huellas de llamada; no
  hay tokens/claves/SDP completos. Mantener `Log` fuera de release y no
  ampliar a payloads.
- **B2. Handshake WS:** aceptar la ausencia de `Sec-WebSocket-Accept` es una
  debilidad de protocolo; TLS lo mitiga. Exigirlo.
- **B3. `FLAG_SECURE`:** capturas/recents de chat y llamada; añadir en
  pantallas sensibles.
- **B4. API key de `google-services.json`:** es clave de cliente, no secreto,
  pero restringir por package/SHA-1 en Google Cloud y no commitear el archivo
  (hoy no está trackeado, pero no está en `.gitignore`).
- **B5. 16 KB:** `libgostsframe.so` y WebRTC alineados; VLC/otras libs no;
  actualizar para Play en dispositivos 16 KB (compatibilidad, no vuln).
- **B6. CI/seguridad:** sin SAST/dependency scanning/tests de autorización;
  añadir CI con `detekt`/`semgrep`/`trivy`, tests de IDOR/rate limit y
  auditoría de dependencias.

---

## Plan de remediación

### 0-7 días (quick wins)

1. C2: desactivar cleartext y añadir pinning (API + WS).
2. A2: `chmod 600 .env`, rotar secretos, `UMask=0077`.
3. A1: rate limit en auth/notify/messages/device/SDP/ICE y cuotas TURN.
4. A3: `allowBackup=false` o excluir prefs sensibles.
5. A5: rehash de hashes legacy al login.
6. M2: notificaciones de mensajes privadas en lockscreen.
7. M3/M4: validar sticker id y call id/chat id en el cliente.
8. B3: `FLAG_SECURE` en chat/llamada.

### 30 días

1. C1/A6: identidad GOST persistente, firma/verificación SDP, TOFU +
   safety number, confirmación de clave y ratchet.
2. A8: derivación Streebog en el fork, zeroización y tests. (C3 RNG/IV ya
   está implementado en `027d75b`; falta validarlo en dispositivo.)
3. A4: pinning con backup pin y validación estricta del handshake.
4. A7: build release firmada + R8; dejar de distribuir debug.
5. M6: FCM servidor (service account) con HTTP v1 y multi-dispositivo.
6. M7: AAR en Maven interno con hash/CI.

### 90 días

1. Auditoría externa de AEAD/VKO/fork/identidad y pentest de API/WS.
2. Multi-ABI/16 KB, reproducible builds.
3. Contactos/bloqueo/reportes anti-spam; privacidad de metadata.
4. SAST/DAST/CI continuo y pruebas de autorización.

## Notas

- La app usa **debug builds** en las pruebas; los hallazgos de release
  (A7) aplican a la distribución.
- El servidor de producción ya expone `/users/device` e `/calls/identity`,
  pero **no** el emisor FCM; el hallazgo M6 es de disponibilidad.
- Este análisis no sustituye una auditoría criptográfica ni un pentest.
