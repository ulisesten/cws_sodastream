# Propuesta: llamadas de voz/video y modo walkie-talkie

> Estado: **propuesta técnica** (sin implementación). Fecha: 2026-11-15.

## 0. Resumen ejecutivo

Se propone añadir llamadas 1:1 de **audio y video** y, en una fase posterior,
un **modo walkie-talkie half-duplex** (push-to-talk, una sola dirección de voz
a la vez). El plano de medios usa **WebRTC** (ICE + DTLS-SRTP), con **P2P**
para 1:1 y **SFU** para grupos/walkie; el servidor C actual queda como
**plano de señalización, autorización y tokens**, sin tocar el media.

La seguridad se plantea como **E2EE obligatoria** (sin fallback a claro),
autenticación verificable, secreto directo (PFS), sin grabación y minimización
de metadatos. Además se introduce un **presupuesto de fiabilidad
(R-Budget)**: cuando el cliente baja calidad/FPS, apaga cámara o entra en
audio-only, los recursos liberados (CPU, ancho de banda, térmico) se
**reinvierten en redundancia** (FEC, RTX, duplicación de audio, pacing), no se
quedan ociosos. La batería y la temperatura se controlan con presets
térmicos/de batería y apagado de pantalla en llamadas de audio.

## 1. Objetivos y no objetivos

### Objetivos
- Llamadas 1:1 de audio y video con **E2EE obligatoria**.
- Controles: mute de micrófono, mute local de participantes, cámara on/off,
  calidad/resolución/FPS, volumen maestro/por participante, ganancia de mic,
  AGC/NS/AEC, ruteo altavoz/auricular/Bluetooth.
- Adaptación a red inestable: audio primero, FEC/RTX/redundancia, ICE restart,
  TURN TCP/TLS, audio-only, reconexión.
- Batería y térmica: códecs hardware, audio-only, pantalla apagada en audio,
  límites de FPS/resolución, menos composición/render.
- **Regla de reinversión**: toda reducción de trabajo en el cliente debe
  aumentar la fiabilidad del envío (dentro del presupuesto de red disponible).
- Walkie-talkie: canal half-duplex con control de turno (floor), PTT, sin
  escucha mientras se habla, optimizado para conexión deficiente.
- Compatibilidad con el modelo actual: Bearer GOST, `usu_id_public`,
  WS `/ws/v1`, SQL Server, sin exponer `usu_id` interno.

### No objetivos (en esta propuesta)
- Grabación de llamadas en servidor (incompatible con E2EE por diseño).
- Transcripción o análisis de contenido en servidor.
- Interoperabilidad con telefonía PSTN/SIP en la primera versión.
- Compartir pantalla, pizarras o llamadas con más de N participantes en la
  primera entrega (se deja como fase posterior).

## 2. Decisiones tomadas

| Tema | Decisión |
|---|---|
| Plataformas | **Android (APK sideload, sin Play Store)**, **Linux** y **Windows**; **web e iOS fuera de alcance por ahora** |
| Alcance | **1:1 primero**; grupo secundario; walkie después |
| Medios | 1:1 P2P; **Janus interno** para grupo/walkie; **gateway C** |
| Ring | **WSS persistente** mientras la app vive; **Telegram bot + deep link** si no; **sin FCM/APNs/WNS** |
| Integración externa | API/webhook HMAC (IP/teléfono/ID) + bot Telegram; “conectar los puntos”, sin CRM |
| Criptografía | **GOST-first**: Kuznyechik/CMAC/Streebog/VKO/34.10-2012; **sin AES/ECDHE/Ed25519 para contenido/identidad** |
| Transporte | DTLS-SRTP estándar (WebRTC/Janus) **solo como tubería**; contenido E2EE GOST; metadatos visibles |
| TLS | **GOST TLS** en el borde si es viable; fallback TLS 1.3 + pinning; **CA privada + pinning** |
| Grupo E2EE | **Sender keys + GOST** |
| Grabación/auditoría | No contenido/llamadas; sí logs/seguridad/auditoría criptográfica |
| Distribución | APK/MSI/AppImage self-hosted; sin Play Store |

### Consecuencias directas
- Sin FCM/APNs, una llamada a app cerrada **no suena sola**: Telegram manda un
  deep link y el usuario lo toca. Si se quiere ring automático, hay que aceptar
  FCM/APNs.
- WebRTC/Janus imponen DTLS-SRTP estándar; si “ni transporte NSA” es requisito
  absoluto, hay que abandonar WebRTC/Janus y construir un stack GOST propio
  (fuera de alcance).
- Telegram es un tercero aceptado **solo para notificación/deep link**; no
  transporta media ni contenido.

### Puntos abiertos (detalle en §15)
1. Perfil GOST: auditoría/constant-time/benchmarks de Kuznyechik/AEAD/HKDF.
2. Aceptación de la frontera DTLS-SRTP estándar + E2EE GOST.
3. Gateway C ↔ Janus: API/eventos/HA/backpressure.
4. Sender keys: epoch/distribución/rotación/multi-dispositivo.
5. WSS persistente + Telegram: fiabilidad, Doze/OEM, escala, deep links.
6. Integración externa: HMAC, mapeo IP/teléfono, webhooks, rate limits.
7. CA privada/pinning/GOST TLS; distribución APK/MSI/AppImage y actualizaciones.
8. Android/Qt+libwebrtc, multi-dispositivo, walkie, UX, calidad, ops y pruebas.

## 3. Arquitectura propuesta

```
                 ┌──────────────────────────────┐
   Apps ────────▶│  cws_sodastream (C)          │
   (señalización │  - auth Bearer/cookie         │
    WSS/REST)    │  - autorización de llamada    │
                 │  - call tokens (TTL corto)    │
                 │  - estado/CDR/rate limits     │
                 │  - entrega de eventos WS      │
                 └───────┬──────────────┬───────┘
                         │              │
              señalización│              │ tokens/ICE
                         ▼              ▼
                 ┌──────────────┐  ┌──────────────┐
                 │ SFU (grupo)  │  │ STUN/TURN    │
                 │ Janus        │  │ coturn       │
                 │ (no descifra)│  │ TLS 443      │
                 └──────┬───────┘  └──────┬───────┘
                        │                 │
                        ▼                 ▼
                 ┌──────────────────────────────┐
                 │  WebRTC P2P (1:1) / SFU     │
                 │  DTLS-SRTP + E2EE por frame  │
                 └──────────────────────────────┘
```

### 3.1 Plano de señalización (C)
- **Clientes**: Android nativo (Kotlin+JNI), Linux y Windows (Qt+C). **Web e
  iOS fuera de alcance** por ahora.
- **Gateway C**: único endpoint público; valida el token GOST y habla con
  Janus por API interna (WS/HTTP). Janus no se expone. Para 1:1 P2P el gateway
  solo relay SDP/ICE/floor; para grupo/walkie traduce a Janus VideoRoom.
- **WSS persistente**: los clientes mantienen una conexión WSS para
  presencia/llamadas; sin APNs/FCM/WNS. Reconnect/backoff, heartbeat y
  refresco de token.
- **Telegram**: bot para enlazar `chat_id` ↔ `usu_id_public` y enviar el
  `call_id` con deep link `cws://call/<id>` cuando no hay WSS. Solo
  notificación; sin media ni contenido.
- **Integración externa**: API/webhook HMAC para mapear IP/teléfono/ID a
  `usu_id_public`, crear/notificar llamadas y recibir eventos (sin contenido).
- `cws_sodastream` sigue siendo el dueño de la identidad y la autorización.
- Nuevos eventos WS para llamada (SDP/ICE/floor/estado) sobre `/ws/v1` para
  clientes nativos.
- La señalización **nunca** transporta media; solo SDP, ICE, estados y
  mensajes de floor.
- Entrega a **todos los dispositivos activos** del usuario; el primer
  `accept` gana y se cancelan los demás (ring simultáneo).

### 3.2 Plano de medios (WebRTC)
- **1:1**: P2P con ICE; TURN como fallback; media directa entre clientes.
- **Grupo/walkie**: SFU; el SFU **reenvía** paquetes y **no descifra** (E2EE
  por frame). No hay mezcla ni transcodificación en servidor.
- **Códecs**: audio Opus; video H.264/VP8/VP9/AV1 según dispositivo. Para SFU
  usar **simulcast** (capas por SSRC), no SVC: el SFU no puede leer los
  descriptores SVC si el payload va cifrado. SVC solo en 1:1 si el stack lo
  permite. Prioridad a códecs con aceleración hardware (MediaCodec, VA-API,
  Media Foundation).
- **Transporte**: DTLS-SRTP; ICE consent freshness; TURN sobre TLS 443 para
  redes restrictivas; ICE restart ante cambio de red.

### 3.3 Topologías
| Escenario | Topología | Motivo |
|---|---|---|
| 1:1 audio/video | P2P + TURN | Menor latencia, sin coste de SFU, más privacidad |
| Grupo pequeño (≤8) | SFU | Fan-out y suscripciones; sin mezcla |
| Grupo mediano (≤32) | SFU + simulcast | Menos subidas por cliente; E2EE no permite SVC |
| Walkie 1:1 | Janus VideoRoom (o P2P) | Janus si se exige half-duplex del lado servidor |
| Walkie canal (N oyentes) | Janus VideoRoom | Un publicador, N oyentes; Janus pausa al que habla |

### 3.4 Servidores auxiliares
- **Janus (elegido)**: self-hosted; plugin **VideoRoom** para grupo y walkie;
  **no** AudioBridge (mezcla incompatible con E2EE); sin grabación, sin
  transcodificación, sin RTP forward. Janus **no descifra**; solo reenvía frames
  cifrados. API **solo interna**: el **gateway C** es el único que habla con
  Janus (el cliente nunca lo ve). Licencia GPLv3: servicio separado; revisar
  obligaciones si se distribuye.
- **1:1 audio/video no usa Janus**: es P2P con ICE/TURN. Janus entra en
  grupo y walkie, así que **no está en la ruta crítica del MVP 1:1**.
- **coturn**: STUN/TURN; `use-auth-secret` con credenciales de tiempo limitado
  emitidas por el servidor C; TLS en 443; `no-multicast-peers`;
  `denied-peer-ip` para rangos privados; cuotas por usuario/IP; `external-ip`
  correcto.
- **WSS persistente**: Android foreground service + batería/Doze;
  Linux/Windows tray/servicio. Sin push de plataforma. Si la app no vive, el
  ring sale por **Telegram bot + deep link** (el usuario lo toca).
- **TLS**: **CA privada + pinning** para apps Android/Linux/Windows. Objetivo
  **GOST TLS** en el borde (OpenSSL + gost-engine) si es viable; fallback TLS
  1.3 + pinning. No hay web que dependa de CA pública.
- **Todo self-hosted**: Janus, coturn, señalización, DB, métricas/logs
  (Prometheus/Grafana/Loki self-hosted), secretos en ficheros/TPM/OS keychain.
  Sin SaaS de llamadas, sin app stores y sin push de plataforma.

### 3.5 Integración con el servidor actual
- No se mete RTP en `cws`; el servidor HTTP no se convierte en media server.
- `ws_v1_emit_to_user` se reutiliza para eventos de llamada; añadir
  `device_id`/`call_id` y entrega multi-dispositivo.
- Los tokens de llamada son **independientes** del access token de 5 min:
  sobre firmado con **GOST R 34.10-2012** (o HMAC-Streebog) con `jti`, `aud`,
  `call_id`, `scope`, `exp` (60 s), `sub` (usuario interno), emitido por el
  servidor C. El **gateway C** valida el sobre GOST y acuña el token estándar
  interno de Janus (60 s); Janus/TURN nunca ven el token GOST. Un token por
  rol/dispositivo; un solo uso para `join`.
- `STICKERS_DIR` y el resto de configuración no se tocan; se añaden variables
  de llamadas (sección 11).

## 4. Seguridad: “100% seguras” con garantías verificables

> “100% seguro” no es una propiedad demostrable. Lo que sí se puede
> garantizar es un conjunto verificable de propiedades criptográficas y de
> diseño. Los riesgos residuales se enumeran en 4.7.

### 4.1 Modelo de amenazas
| Amenaza | Mitigación |
|---|---|
| Pasivo en red | DTLS-SRTP + E2EE; sin RTP en claro |
| MITM en señalización | GOST TLS o TLS 1.3 + pinning; tokens GOST; verificación de claves E2EE |
| Servidor/SFU malicioso | E2EE por frame; el SFU no tiene claves; sin grabación |
| Participante malicioso | E2EE de grupo (sender keys GOST), autenticación, floor firmado |
| Robo de dispositivo | Keystore/DPAPI/TPM, biometría, sin claves en logs, expiración |
| Replay | `jti` de un solo uso, nonces, secuencias, ventanas de replay |
| Spam/abuso | Solo contactos, bloqueo, rate limits, límite de llamadas |
| DoS | Cuotas TURN, timeouts de ring, límite de participantes, rate limits |
| Metadatos | Minimización, retención corta, sin contenido, cifrado en reposo |

### 4.2 E2EE
> **Estado actual de `c_gost_encryption`**: implementa Streebog-256/512,
> HMAC-Streebog, PBKDF2, GOST R 34.10-2012 (firma), VKO
> (R 50.1.113-2016), curvas 2012, Magma (GOST 28147-89) y, desde
> `b436423`, **Kuznyechik** (GOST R 34.12-2015), **CTR-ACPKM + CMAC**
> (GOST R 34.13-2015), **HKDF-Streebog**, **AEAD** y **SFrame-GOST**.
> Pendiente: auditoría externa, garantías constant-time, benchmarks en
> Android/Windows e integración probada desde el cliente. Ver §16.3.

- **1:1**: X3DH adaptado a GOST:
  - identidades y prekeys firmadas con **GOST R 34.10-2012**;
  - DH con **VKO** (R 50.1.113-2016) en curvas TC26;
  - KDF **HKDF-Streebog** (implementado sobre HMAC-Streebog);
  - Double Ratchet con HMAC-Streebog y VKO; PFS y post-compromise.
- **Frame crypto**: perfil **SFrame-GOST** (cabecera con KID/contador + AEAD
  propio; no interoperable con SFrame estándar, todos los clientes son
  propios):
  - **Implementado (decisión GOST-first)**: **Kuznyechik-CTR-ACPKM + CMAC**
    (GOST R 34.12/34.13-2015), con vectores de prueba. **Sin
    AES-GCM/ChaCha20** para contenido.
  - **Fallback temporal**: **Magma-gamma + HMAC-Streebog-256**
    (Encrypt-then-MAC) si el rendimiento de Kuznyechik en software no alcanza
    en un dispositivo; se documenta y se mide.
  - No usar el `gost_hash` one-shot (variante reversa del proyecto) para
    cripto; usar la API streaming RFC 6986 y `gost_hmac`.
- **Frontera de transporte**: WebRTC/Janus imponen **DTLS-SRTP estándar**; el
  contenido va cifrado con GOST (E2EE). Si se exige que ni el transporte use
  estándares “NSA”, hay que abandonar WebRTC/Janus y construir un stack GOST
  propio (fuera de alcance de esta propuesta).
- **Grupo (decisión)**: **sender keys + ratchet/epoch con GOST**, distribuidas
  por los canales 1:1; rotación al entrar/salir; multi-dispositivo; **sin
  MLS**. Requiere spec propia y pruebas de seguridad (epoch, replay, late
  join, revocación).
- **Verificación**: safety number/QR con Streebog; fingerprint de epoch;
  “verificado” solo tras comparación fuera de banda.
- **Sin fallback**: `CALL_E2EE_REQUIRED=true`; si un extremo no soporta E2EE,
  la llamada falla.
- **SFU**: Janus reenvía frames cifrados; no descifra, no graba, no
  transcodifica. Para grupo usar **simulcast** (SSRC por capa), **no SVC**,
  porque el SFU no puede leer descriptores SVC cifrados.
- **Auditoría**: VKO/X3DH-GOST/SFrame-GOST son criptografía propia; requiere
  vectores de prueba, revisión de canal lateral y pentest antes de llamarla
  “100%”.

### 4.3 Señalización y tokens
- WSS con **GOST TLS** en el borde si es viable; fallback TLS 1.3; HSTS; certificate pinning en apps.
- Access token Bearer actual (GOST) para autenticar la sesión de señalización.
- **Call token** firmado con **GOST R 34.10-2012** (o HMAC-Streebog si el
  borde lo exige): `iss`, `aud`, `sub`, `call_id`, `device_id`, `scope`
  (`join`, `publish`, `subscribe`, `floor`), `jti`, `exp` (60 s). Un solo
  uso; revocable; fuera de logs.
- **Frontera GOST/estándar (decisión)**: **gateway C** valida el token GOST
  y acuña un token estándar de corta vida para Janus/TURN; Janus no se expone.
  coturn sigue usando HMAC de tiempo limitado (inevitable); documentar.
- **Ring token** para el deep link de Telegram (app no conectada): TTL 60 s, scope mínimo (`call.read`, `call.join`), un solo uso.
- Autorización: solo contactos / política de privacidad; bloquear; límite de
  llamadas concurrentes; rate limit por IP/usuario.
- Validación estricta de SDP/ICE/tamaños; timeouts; límites de participantes.

### 4.4 Transporte
- **DTLS-SRTP estándar (AES-GCM)** es obligatorio en WebRTC/Janus; **no hay
  perfil GOST práctico** en el stack. El contenido va E2EE GOST; el transporte
  es solo una tubería y no ve el contenido. Metadatos (IPs, puertos, tamaños,
  tiempos) sí quedan expuestos al transporte; documentarlo.
- **TLS de señalización/API**: objetivo **GOST TLS** (OpenSSL + gost-engine)
  en el borde; fallback TLS 1.3 + pinning; **CA privada + pinning**. Sin web
  no hay dependencia de CA pública.
- TURN sobre TLS 443 (`turns:`) para redes que bloquean UDP; STUN solo para
  descubrimiento.
- **Privacidad de IP**: modo “relay-only” opcional (`iceTransportPolicy:
  relay`) para no exponer IPs a pares; por defecto P2P con fallback TURN.
  mDNS host candidates cuando aplique.
- **WSS persistente**: sin push de terceros; heartbeat, backoff, refresco de
  token y reconexión; en Android depende de foreground service/Doze.
- Sin RTP en claro; sin SRTP con claves débiles; sin downgrade.

### 4.5 Privacidad y metadatos
- El servidor conoce **quién llama a quién, cuándo y cuánto dura** (CDR), no
  el contenido. Minimizar, cifrar en reposo, retención corta, acceso auditado.
- Sin grabación, sin transcripción, sin analítica de contenido.
- Logs sin tokens, sin SDP completo, sin IPs completas (o retención mínima).
- Ring por WSS persistente; si no hay conexión, Telegram bot manda `call_id` + deep link (sin contenido). El servidor no retiene contenido.
- Política de privacidad clara: qué metadatos se guardan y por qué.

### 4.6 Cliente
- Claves GOST cifradas con **Kuznyechik + PBKDF2-Streebog** (passphrase/
  secreto de dispositivo) y, opcionalmente, ligadas a Keystore/DPAPI/TPM para
  anti-extracción; nunca en texto plano.
- Permisos de mic/cámara just-in-time; indicadores del sistema; parada real de
  captura al apagar cámara/mic (hard mute).
- `FLAG_SECURE` (Android) / ocultar previews en el app switcher; en
  escritorio, evitar capturas si el SO lo permite.
- **CA privada + pinning**: Android Network Security Config, Qt SSL,
  almacén de certificados de Windows; pins de respaldo, rotación y revocación.
- Sin captura en background salvo llamada activa (foreground service /
  background audio con micrófono).
- Actualizaciones: dependencias fijadas, SBOM, escaneo de vulnerabilidades.

### 4.7 Lo que no se puede garantizar
- Extremo comprometido (malware, OS, cámara/mic físicos).
- Metadatos visibles para el servidor (quién/llamada/duración); sin proveedores de push por defecto.
- Ataques de denegación de servicio.
- Compromiso de la cadena de suministro del dispositivo/SDK.
- “100%” debe interpretarse como **E2EE verificable + mínima confianza**.

## 5. Flujo de llamada

### 5.1 1:1
1. `POST /api/v1/calls` con `usu_id_public`, `tipo` (`audio|video|walkie`).
2. Servidor valida contacto/bloqueo/estado, crea `call_id_public`, emite
   `call_token` y devuelve ICE/TURN.
3. Evento WS `call.incoming` al receptor por **WSS persistente**. Si no hay
   conexión, el bot de Telegram manda el `call_id` + deep link
   `cws://call/<id>`; si el usuario no lo toca, se registra missed call.
4. Receptor `accept`/`reject`; el primero gana; se cancelan otros dispositivos.
5. Intercambio SDP/ICE por WS; WebRTC conecta; E2EE activa desde el primer
   frame; si falla la verificación, se corta.
6. `end`/timeout/error → CDR; liberar tokens y sala.
7. Ring timeout 30-45 s; busy; missed.

### 5.2 Grupo
1. `POST /api/v1/calls` con `grupo: true` y participantes.
2. Servidor crea sala en SFU y emite tokens `join` por participante.
3. Cada cliente publica audio/video y se suscribe según topología/simulcast (SVC solo 1:1).
4. E2EE de grupo: sender keys + epoch; al entrar/salir se rota.
5. Calidad por participante (volumen local, mute local, video on/off).

### 5.3 Walkie
Ver sección 8. La llamada puede nacer como `walkie` o entrar en modo walkie
desde una llamada de audio cuando la red se degrada (decisión del usuario o
automática con confirmación).

## 6. Controles de usuario

### 6.1 Audio
- **Mute de micrófono**: soft mute (`track.enabled=false`) o **hard mute**
  (parar captura y liberar micrófono). Default hard mute por privacidad;
  opción “mute rápido” para latencia.
- **Mute local de participante**: silencia solo en el dispositivo, sin
  afectar a los demás.
- **Mute de salida**: silencia todo el audio recibido.
- **Volumen**: maestro 0-200 %, por participante 0-200 %, ganancia de mic
  (AGC on/off), limitador para evitar clipping, normalización opcional.
- **Ruteo**: altavoz, auricular, Bluetooth, headset; respetar audio focus y
  llamadas del sistema.
- **Procesamiento**: AEC, NS, AGC configurables; por defecto AEC/NS on.

### 6.2 Video
- **Cámara on/off**: al apagar, parar captura (hard off) y enviar frame
  negro/congelado; indicador visible; al encender, readquirir cámara.
- **Pausa de video** (freeze) sin apagar cámara: deja de codificar/enviar;
  útil para ahorrar batería sin reabrir el dispositivo.
- **Cámara frontal/trasera**; espejo local; rotación; PiP.
- **Preview local** on/off (apagarlo ahorra GPU/batería).
- **Calidad**: resolución (180p/360p/720p), FPS (15/24/30), bitrate máximo,
  intervalo de keyframe; botón “forzar keyframe”.
- **Modo datos**: prioriza audio; video opcional.

### 6.3 Calidad y fotogramas
- Presets: **Auto**, **Ahorro**, **Balanceado**, **HD**, **Solo audio**.
- Control manual con feedback: al bajar FPS/resolución se muestra qué se
  gana (FEC/redundancia) y qué se pierde (nitidez).
- **Controlador adaptativo**: transporte (RTT, pérdida, jitter, BWE) +
  dispositivo (térmica, batería, CPU) + preferencia del usuario.
- Histéresis para evitar oscilación; audio siempre por encima de video.
- “Congelar video” antes de degradar audio.

### 6.4 Volumen y dispositivos
- Slider de volumen de salida y de micrófono; medidores de nivel.
- Selector de dispositivo de entrada/salida; recordar por ruta (Bluetooth,
  auricular).
- En grupo: volumen por participante y “mute local” por participante.

## 7. Optimización: red inestable, batería y térmica

### 7.1 Presupuesto de fiabilidad (R-Budget)
La idea central: **el ahorro no se queda idle**.

```
B_total   = min(BWE_estimado * 0.9, cap_dispositivo)
B_media   = B_total - overhead
overhead  = FEC + RED + RTX + cabeceras
ahorro    = B_total - B_media_actual
```

- Si el usuario baja FPS/resolución o apaga cámara, `B_media` baja y
  `ahorro` sube.
- Ese ahorro se reinvierte, **solo si hay margen y la pérdida es aleatoria**,
  en:
  - audio: Opus RED (2-3 copias), FEC in-band, duplicación de frames
    críticos, pacing más fino;
  - video: ULPFEC/FlexFEC, RTX, keyframes redundantes;
  - cifrado/pacing: la CPU liberada se usa para FEC y para enviar más
    frecuentemente (menos ráfagas).
- **Regla anti-congestión**: si RTT/cola suben, es congestión → **bajar
  media**, no añadir FEC. Si la pérdida es aleatoria con RTT estable → añadir
  FEC/duplicación. El controlador clasifica el tipo de pérdida.
- **Goodput, no conteo bruto**: “maximizar el envío” significa maximizar
  paquetes **útiles entregados** y continuidad de audio, no inflar paquetes.

### 7.2 Control de congestión y redundancia
- `transport-cc` + GCC; pacer; NACK/RTX; jitter buffer adaptativo.
- Audio: Opus 48 kHz, 20 ms, DTX, in-band FEC, RED; 6-40 kbps voz.
- Video: simulcast (H.264/VP8/VP9) para SFU; SVC solo 1:1; 15-30 fps; 180-720p;
  100-2500 kbps; keyframe 2-5 s; `degradationPreference` balanceado.
- Prioridad de audio en el pacer; audio nunca se corta por video.
- Red muy mala: audio-only, 6-12 kbps, 40 ms, 2-3x redundancia; video freeze;
  TURN TCP/TLS; ICE restart; reintentos con backoff.
- Métricas: RTT, pérdida, jitter, BWE, FEC recuperado, RTX, NACK, bitrate,
  frames descartados, audio concealment.

### 7.3 Batería y térmica
Entradas: nivel/carga de batería, térmica del sistema, CPU, pantalla on/off,
red y preferencia. APIs: `getCurrentThermalStatus`
(Android), `thermal_zone`/`power_supply` (Linux), `GetSystemPowerStatus` +
`ThermalZone`/WMI (Windows). En escritorio además `SetThreadExecutionState`
(Windows) y DPMS/inhibit (Linux) para no apagar la pantalla en video, y
permitir apagarla en audio.

Acciones:
- **Audio-only** y **pantalla apagada** en llamadas de audio (proximity/
  audio focus); no mantener wakelock innecesario.
- **Video**: apagar preview local, limitar FPS (15/24), resolución (360p),
  desactivar filtros/efectos, UI oscura y sin animaciones, composición mínima
  (SurfaceView/hardware overlay), no 60 fps.
- **Térmica serious/critical**: bajar a 360p/15 o audio-only; avisar al
  usuario; no matar la llamada de audio.
- **Batería <20 %** (sin cargador): audio-only por defecto; **<10 %**: audio
  de bajo bitrate y sin video.
- Códecs **hardware**; evitar software; liberar cámara al apagar; parar
  stats/timers de UI; batchear señalización.
- Medir mAh/min y temperatura; no optimizar a ciegas.

### 7.4 Tabla de estados (ejemplo)

| Estado | Red | Térmica/Batería | Video | Audio | Redundancia |
|---|---|---|---|---|---|
| Normal | Buena | Nominal | 720p/30 | 32 kbps | FEC 10 %, RTX |
| Reducido | Pérdida 5-10 % | Fair | 360p/24 | 24 kbps | FEC 20 %, RED 2x |
| Agresivo | RTT alto/pérdida 10-20 % | Serious | 360p/15 o freeze | 16 kbps | FEC 30 %, RED 3x |
| Radio | Pérdida >20 %/RTT >500 ms | Critical/<10 % | Off | 6-12 kbps, 40 ms | 3x + FEC, TURN TCP |
| Walkie | Cualquiera | Cualquiera | Off | 6-16 kbps | 3x, PTT |

### 7.5 Medición
- Batería: mAh/min por modo (audio, video 360p/15, 720p/30).
- Térmica: temperatura y throttling durante 10-15 min.
- Calidad: POLQA/MOS estimado, audio concealment, frames descartados.
- Red: pérdida, RTT, jitter, BWE, FEC overhead, goodput.
- Latencia: setup, join, PTT→primer audio, floor grant.

### 7.6 Llamadas largas, larga distancia y red inestable
- **Audio-first** para duración: Opus 16-24 kbps, 20 ms, DTX, FEC/RED 2-3x,
  PLC, jitter buffer 80-200 ms; video opcional 360p/15; audio-only si
  batería/térmica/red lo piden.
- **R-Budget** durante toda la llamada: el ahorro de video/FPS se reinvierte
  en redundancia de audio; nunca se deja idle.
- **Larga distancia**: TURN/Janus self-hosted en varias regiones; elegir el
  relay más cercano por RTT; P2P primero; relay-only en redes restrictivas;
  ICE restart y migración WiFi↔LTE.
- **Inestable**: bajar bitrate, subir FEC/RED, TURN TCP/TLS 443, jitter buffer
  adaptativo, PLC, reconexión/ICE restart; walkie como fallback.
- **Larga duración**: heartbeat WSS, refresh de tokens, keepalive TURN/Janus,
  watchdog de memoria/fds, renegociación periódica, límites de sesión,
  monitorización de mAh/min y temperatura; evitar leaks.
- **Batería/térmica**: pantalla apagada en audio; preview off; hardware
  codecs; software GOST medido; presets térmicos sostenidos (no solo picos).
- **Servidor**: sesiones largas en Janus/gateway, allocaciones TURN, HA,
  timeouts, cuotas y métricas; capacidad de relay por región.
- **Medición**: llamadas de 1-4 h en 3G/4G/5G/WiFi, pérdida 5-30 %, RTT
  100-800 ms, cambio de red, batería y térmica.

## 8. Modo walkie-talkie (half-duplex)

### 8.1 Modelo
- Canal de voz con **un solo turno de habla** a la vez.
- El que habla **no escucha** mientras mantiene pulsado (requisito).
- Los oyentes escuchan al que tiene el turno.
- **Janus VideoRoom (audio-only)** para walkie: un publicador, N oyentes;
  permite pausar suscripciones y garantizar half-duplex del lado servidor.
  1:1 puede ser P2P, pero para garantía estricta se usa Janus también en 1:1.
- Se activa como tipo de llamada `walkie` o como **modo radio** dentro de una
  llamada de audio cuando la red es deficiente.
- Grupo es secundario; walkie 1:1 primero, canal después.

### 8.2 Control de turno (floor)
Mensajes de señalización autenticados por la sesión y firmados/ligados al
`call_token`:
- `call.floor_request` → `call.floor_granted` / `call.floor_denied`.
- `call.floor_release`.
- `call.floor_revoked` (timeout, desconexión, fin de llamada).

Reglas:
- El servidor serializa las solicitudes; gana la primera.
- Lease de 1-2 s renovable mientras se mantiene pulsado; si expira, se
  revoca.
- Si el canal está ocupado: `busy` con `retry_after`; opción de cola simple.
- Al liberar, el siguiente puede pedir; no hay solapamiento.
- El cliente del hablante **silencia la reproducción** (no escucha). En
  Janus el servidor además **pausa la suscripción** de los demás hacia el
  hablante (ahorra ancho de banda y lo garantiza del lado servidor); el
  silencio local es defensa en profundidad.
- Si el hablante se desconecta, auto-release por timeout.

### 8.3 Audio
- Opus mono 16 kHz, 20 ms, 6-16 kbps, DTX off mientras se habla (para no
  perder el inicio), in-band FEC, 2-3 copias/redundancia.
- Captura “caliente” (track ya creado, `enabled=false`); al pulsar, habilitar
  y enviar de inmediato; pre-roll mínimo si la latencia del floor lo exige.
- **PTT optimista** (opcional): si el canal está libre, empezar a enviar y
  pedir floor en paralelo; si se deniega, descartar y avisar. Reduce la
  latencia percibida a costa de un pequeño riesgo de solapamiento.
- Jitter buffer bajo (40-80 ms) para PTT; PLC para pérdidas.
- Sin video.

### 8.4 Red deficiente
- Bajar a 6-8 kbps, 40 ms, 3x redundancia; TURN TCP/TLS; ICE restart.
- Si RTT >1 s o pérdida >30 %: aviso “conexión deficiente”; opción de
  **nota de voz** (grabar y enviar por el chat) como fallback, fuera del
  alcance inicial.
- El modo walkie prioriza **continuidad** sobre fidelidad.

### 8.5 Seguridad
- Floor y audio bajo la misma sesión E2EE; mensajes de floor autenticados y
  con nonce/secuencia; no se puede robar el turno ni inyectar audio.
- El SFU no descifra; solo reenvía al hablante autorizado.
- El token de floor tiene alcance y TTL cortos; revocable.

## 9. API y eventos

### 9.1 Endpoints REST (borrador)
| Método | Ruta | Estado |
|---|---|---|
| POST | `/api/v1/calls` | Pendiente como alias; hoy el alta es `/api/v1/integrations/telegram/notify` |
| GET | `/api/v1/calls/:call_id` | Implementado (estado/rol/peer) |
| POST | `/api/v1/calls/:call_id/accept` | Pendiente como alias; hoy `/integrations/telegram/accept` |
| POST | `/api/v1/calls/:call_id/reject` | Pendiente como alias; hoy `/integrations/telegram/reject` |
| POST | `/api/v1/calls/:call_id/end` | Pendiente como alias; hoy `/integrations/telegram/end` |
| POST | `/api/v1/calls/:call_id/floor` | Pendiente (walkie) |
| GET | `/api/v1/calls/:call_id/ice` | Implementado: STUN/TURN temporales |
| POST | `/api/v1/calls/:call_id/sdp` | Implementado: relay `call.sdp` |
| POST | `/api/v1/calls/:call_id/ice` | Implementado: relay `call.ice` |
| GET | `/api/v1/calls/history` | Pendiente (CDR) |
| POST | `/api/v1/calls/:call_id/token` | Pendiente (Janus/grupo) |
| POST | `/api/v1/integrations/telegram/link` | Enlazar Telegram (código de un solo uso) |
| POST | `/api/v1/integrations/external/calls` | Crear/notificar llamada externa (HMAC) |
| POST | `/api/v1/integrations/external/link` | Mapear IP/teléfono/ID a `usu_id_public` (HMAC) |
| POST | `/api/v1/integrations/external/webhook` | (salida) eventos de llamada al sistema externo |

Autenticación: Bearer (Android/desktop). `usu_id_public` en body, nunca
`usu_id` interno. Los endpoints de integración usan HMAC + timestamp/nonce +
allowlist, nunca la sesión de usuario. Errores JSON consistentes con el API.

### 9.2 Eventos WS
Cliente → servidor:
`call.accept`, `call.reject`, `call.end`, `call.sdp`, `call.ice`,
`call.quality`, `call.floor_request`, `call.floor_release`, `call.stats`.

Servidor → cliente:
`call.incoming`, `call.ringing`, `call.accepted`, `call.rejected`,
`call.ended`, `call.sdp`, `call.ice`, `call.floor_granted`,
`call.floor_denied`, `call.floor_released`, `call.participant_joined`,
`call.participant_left`, `call.mute_changed`, `call.camera_changed`,
`call.quality_changed`, `call.error`.

Envelope:
```json
{
  "event": "call.sdp",
  "call_id": "abc123XYZ",
  "seq": 17,
  "ts": 1763251200000,
  "from": "usu_public_emisor",
  "to": "usu_public_receptor",
  "data": { "type": "offer", "sdp": "v=0..." }
}
```
Idempotencia por `seq`; `ack` opcional; se descartan duplicados.

### 9.3 Ejemplo de creación
```http
POST /api/v1/calls
Authorization: Bearer <access_token>
Content-Type: application/json

{
  "usu_id_public": "V1StGXR8_Z5j",
  "tipo": "video",
  "grupo": false
}
```
```json
{
  "success": true,
  "data": {
    "call_id": "9f2c1a7b4e",
    "estado": "ringing",
    "call_token": "gost:...",
    "expires_in": 60,
    "ice_servers": [
      { "urls": ["stun:stun.sodastream.fun:3478"] },
      {
        "urls": ["turns:turn.sodastream.fun:5349?transport=tcp"],
        "username": "1763251200:usuario",
        "credential": "base64-hmac"
      }
    ],
    "e2ee": { "required": true, "suite": "x3dh-gost/sframe-gost" }
  }
}
```

## 10. Modelo de datos (SQL Server)

Convención del proyecto: prefijo de tabla, PK `prefijo_id INT IDENTITY`,
`*_id_public` nanoid, sin exponer IDs internos, sin FKs (consistencia
con chat).

### `opr_llamadas` (CDR, sin contenido)
| Columna | Tipo | Notas |
|---|---|---|
| `lla_id` | INT IDENTITY PK | interno |
| `lla_id_public` | CHAR(12) UNIQUE | expuesto |
| `lla_id_emisor` | INT | interno |
| `lla_id_receptor` | INT NULL | NULL en grupo |
| `lla_tipo` | TINYINT | 0 audio, 1 video, 2 walkie, 3 grupo audio, 4 grupo video |
| `lla_origen` | TINYINT | 0 app, 1 sistema externo, 2 Telegram |
| `lla_estado` | TINYINT | 0 ringing, 1 active, 2 ended, 3 missed, 4 rejected, 5 failed, 6 busy |
| `lla_id_sala` | VARCHAR(64) NULL | sala SFU |
| `lla_inicio` | DATETIME2 | |
| `lla_fin` | DATETIME2 NULL | |
| `lla_motivo_fin` | VARCHAR(64) NULL | timeout/end/error/... |
| `lla_creado` | DATETIME2 | default sysutcdatetime |

### `opr_llamadas_participantes`
| Columna | Tipo | Notas |
|---|---|---|
| `llp_id` | INT IDENTITY PK | |
| `llp_id_llamada` | INT | |
| `llp_id_usuario` | INT | interno |
| `llp_rol` | TINYINT | 0 emisor, 1 receptor, 2 invitado |
| `llp_estado` | TINYINT | 0 invitado, 1 conectado, 2 salió, 3 rechazó |
| `llp_entrada` | DATETIME2 NULL | |
| `llp_salida` | DATETIME2 NULL | |

### `opr_llamadas_calidad` (agregado, opcional)
Métricas por llamada/participante: pérdida, RTT, jitter, bitrate, resolución,
FPS, FEC, térmica/batería. **Sin contenido**. Retención corta.

### `opr_llamadas_integracion_externa` (mapeo, sin contenido)
| Columna | Tipo | Notas |
|---|---|---|
| `lie_id` | INT IDENTITY PK | |
| `lie_id_usuario` | INT | interno |
| `lie_tipo` | TINYINT | 0 Telegram, 1 teléfono, 2 IP, 3 ID externo |
| `lie_valor` | VARCHAR(255) | chat_id, teléfono, IP o ID |
| `lie_activo` | BIT | |
| `lie_creado` | DATETIME2 | |

Índice único (`lie_tipo`, `lie_valor`). Sin contenido de llamadas.

SPs: `procLlamadasProc` (crear/actualizar/terminar), `procLlamadasCons`
(estado/historial), `procLlamadasParticipantesProc/Cons`,
`procLlamadasIntegracionExternaProc/Cons`. El usuario aplica los SQL como en
chat/stickers.

## 11. Configuración (.env, borrador)

```dotenv
CALLS_ENABLED=true
CALL_E2EE_REQUIRED=true
CALL_PRIVACY_RELAY_ONLY=false
CALL_RING_TIMEOUT_MS=45000
CALL_FLOOR_LEASE_MS=1500
CALL_MAX_PARTICIPANTS=16
CALL_TOKEN_TTL_S=60
CALL_TOKEN_MODE=gost
CALL_TOKEN_SIGNING_KEY=...        # GOST R 34.10-2012
STUN_URLS=stun:stun.sodastream.fun:3478
TURN_REGIONS=eu:turn-eu.sodastream.fun,us:turn-us.sodastream.fun
TURN_SECRET=...
JANUS_URL=wss://janus.internal
JANUS_API_SECRET=...
JANUS_MODE=gateway
JANUS_TOKEN_TTL_S=60
GOST_FRAME_SUITE=kuznyechik-cmac|magma-hmac256
WSS_PERSISTENT_ENABLED=true
WSS_HEARTBEAT_S=25
WSS_RECONNECT_MAX_S=30
TELEGRAM_BOT_TOKEN=...
TELEGRAM_WEBHOOK_SECRET=...
EXTERNAL_API_SECRET=...
EXTERNAL_API_ALLOWLIST=...
TLS_MODE=gost|tls13
TLS_CA_FILE=/etc/cws/private-ca.pem
TLS_PINS=sha256/...
```
No hay `PUSH_*`: se usa **WSS persistente** + **Telegram deep link**.
No hay Play Store: APK directo. Web/iOS fuera de alcance. `TLS_MODE=gost`
usa GOST TLS en el borde (objetivo); `tls13` es fallback con CA privada +
pinning. `TURN_SECRET`/`JANUS_API_SECRET` nunca se envían al cliente.

## 12. Plan de implementación

| Fase | Entregable | Estimación |
|---|---|---|
| F0 | Modelo de amenazas; spike Android + Linux + Windows 1:1 audio P2P + coturn + E2EE SFrame-GOST (Kuznyechik); gateway C ↔ Janus; GOST TLS/CA privada/pinning | 3-4 semanas |
| F1 | 1:1 audio Android + Linux/Windows: REST/WS, tokens, WSS persistente, Telegram link, API externa, mute, volumen, audio-only, CDR | 4-5 semanas |
| F2 | 1:1 video Android + Linux/Windows: cámara, calidad/FPS, hardware, batería/térmica | 4-6 semanas |
| F3 | R-Budget, llamadas largas/larga distancia, TURN por región, FEC/RTX/RED, ICE restart, TURN TCP, watchdog | 4-5 semanas |
| F4 | Grupo (gateway C + Janus VideoRoom): sender keys + GOST, participantes, volumen por persona | 4-6 semanas |
| F5 | Walkie (gateway C + Janus): floor, half-duplex, PTT, red deficiente | 3-4 semanas |
| F6 | Hardening: auditoría cripto, pentest, carga/soak, APK/packages, rollout | 3-4 semanas |

Supone 2-3 ingenieros con experiencia en WebRTC/móvil, cripto y un responsable
de infra. Ajustar tras el spike. El MVP (Android + Linux/Windows, 1:1
audio/video) no depende de Janus; Janus entra en F4/F5. Web/iOS quedan fuera.

## 13. Pruebas y criterios de aceptación

### Red (tc/netem)
- Perfiles: 3G/4G/5G, RTT 50/200/500 ms, jitter 0/30/100 ms, pérdida
  1/5/10/20/30 %, ancho 64/128/512 kbps.
- Cambio de red (WiFi↔LTE), ICE restart, TURN TCP.

### Batería/térmica
- 10-15 min por modo y llamadas de 1-4 h; medir mAh/min y temperatura.
- Throttling simulado (Android `thermalservice`, Linux `thermal_zone`,
  Windows `ThermalZone`).

### Seguridad
- Vectores GOST (Kuznyechik/CMAC/Streebog/VKO/34.10-2012); canal lateral.
- MITM de señalización; SFU captura tráfico y no puede descifrar.
- Replay de call/ring/floor tokens; downgrade E2EE; spoof de floor.
- HMAC de integración externa/Telegram; replay de webhooks; rate limits.
- Fuzz de SDP/ICE/JSON; abuso de TURN; DoS.

### Integración y distribución
- Telegram: enlace de cuenta, deep link `cws://call/<id>`, app cerrada,
  expiración; sin FCM.
- API externa: mapeo IP/teléfono/ID, HMAC, timestamp/nonce, allowlist,
  webhooks y reintentos.
- APK sideload (firma, actualización self-hosted), MSI/portable y
  AppImage/deb; CA privada/pinning.

### Aceptación (mínimos)
- Setup 1:1 <2 s; join <1 s; PTT→primer audio <300 ms (red buena).
- Audio inteligible con 10 % de pérdida; walkie usable con 20-30 %.
- E2EE: el SFU solo ve ciphertext; sin fallback a claro.
- Audio-only: pantalla apagada, consumo medible y sin throttling.
- Video 360p/15 sostenido sin sobrecalentamiento crítico.
- Walkie: sin double-talk, hablante no escucha, release correcto.
- Llamada de 1-4 h estable; audio-only con pantalla apagada y batería medida.
- Telegram deep link funciona con app cerrada; sin FCM.
- Tokens de un solo uso; replay rechazado.

## 14. Riesgos y mitigaciones

| Riesgo | Mitigación |
|---|---|
| Complejidad E2EE de grupo | Empezar 1:1; grupo con sender keys + GOST; spec y auditoría |
| E2EE + SFU: no transcodificar | Simulcast (no SVC) en SFU; forwarding; validar en spike |
| Cripto GOST (constant-time/auditoría/benchmarks) | Kuznyechik/CMAC/HKDF/AEAD/SFrame implementados; auditar, medir y mantener Magma como fallback |
| DTLS-SRTP estándar en el transporte | Aceptarlo como tubería + E2EE GOST; si no, stack GOST propio (fuera de alcance) |
| Sin push: app cerrada no suena sola | WSS persistente + Telegram deep link; documentar que no es ring automático |
| Telegram es tercero | Solo notificación/deep link; sin media ni contenido; HMAC de webhooks |
| WSS persistente poco fiable (Doze/OEM/batería) | Foreground service, heartbeat/backoff, reintentos; medir batería |
| CA privada / GOST TLS | Jerarquía, pins de respaldo, rotación, revocación; spike GOST TLS, fallback TLS 1.3 |
| APK sideload y actualizaciones | Firma, update self-hosted, verificación; sin Play Store |
| Gateway C ↔ Janus (complejidad C/JSON/WS) | Spike, libs probadas, HA, backpressure; Janus no expuesto |
| Qt+C ↔ libwebrtc (C++) | Puente C/C++ delgado, ownership/event loop, build reproducible |
| Llamadas largas / larga distancia | Audio-first, R-Budget, watchdog, TURN por región, ICE restart; medir 1-4 h |
| Batería/térmica | Audio-only, hardware, límites, pantalla off; medir por dispositivo |
| Coste TURN/SFU | Relay solo cuando haga falta; cuotas; regiones |
| Abuso/spam | Solo contactos, rate limits, bloqueo, tokens cortos |
| “100 % seguro” mal interpretado | Documentar garantías y riesgos residuales |
| Web/iOS | Fuera de alcance por ahora; no se implementan |

## 15. Puntos abiertos (para cerrar antes de F0)

### Decisiones cerradas
Android (APK sideload, sin Play Store) + Linux + Windows; web/iOS fuera. 1:1
primero. Janus interno con **gateway C + token GOST**; **sender keys + GOST**;
**WSS persistente + Telegram deep link** (sin FCM/APNs/WNS); **CA privada +
pinning** y objetivo **GOST TLS**; **GOST-first** (Kuznyechik/VKO/34.10-2012/
Streebog); sin grabación ni auditoría de contenido (sí auditoría criptográfica).

### Bloqueantes
1. **Cripto GOST**: ya implementados Kuznyechik/CTR-ACPKM/CMAC/HKDF/AEAD/
   SFrame-GOST; faltan auditoría externa, garantías constant-time, benchmark
   Android/Windows y prueba de integración desde el cliente. Magma EtM solo
   como fallback medido.
2. **Frontera de transporte**: aceptar DTLS-SRTP estándar como tubería con
   E2EE GOST. Si no se acepta, hay que cambiar el stack (sin WebRTC/Janus).
3. **Gateway C ↔ Janus**: WS/HTTP/JSON, sesiones/plugins/eventos, tokens,
   backpressure, HA y tests; Janus no expuesto.
4. **Sender keys + GOST**: epoch, distribución pairwise, rotación, late join,
   revocación y multi-dispositivo; spec y auditoría.
5. **WSS persistente + Telegram**: Android foreground/Doze/OEM; escala del
   servidor C; app cerrada (deep link, no ring automático); sin FCM.
6. **Integración externa**: HMAC, mapeo IP/teléfono/ID, webhooks, rate limits,
   enlace de Telegram y expiración de deep links.
7. **CA privada / GOST TLS / pinning**: jerarquía, distribución, rotación,
   pins de respaldo, revocación, Android NSC y Qt SSL; viabilidad de GOST TLS.
8. **Distribución**: APK firmado + update self-hosted; MSI/portable y
   AppImage/deb; firma de código; sin Play Store.
9. **Qt+C ↔ libwebrtc**: puente C/C++, ownership/event loop, captura/audio,
   códecs hardware, empaquetado Windows/Linux.
10. **Android**: foreground service, Telecom/ConnectionService, permisos,
    Doze, Keystore, MediaCodec, térmica, minSdk/APK.
11. **Multi-dispositivo**: ring a todos/acepta uno; prekeys por dispositivo;
    revocación; safety numbers; sender keys por dispositivo.
12. **Walkie**: 1:1 vs canal; cola/prioridad de floor; PTT hardware; pantalla
    apagada; fallback a nota de voz; gateway/Janus.
13. **Llamadas largas/calidad/térmica**: objetivos 1-4 h, matriz de
    dispositivos, TURN por región, R-Budget, mAh/min y temperatura.
14. **Ops/seguridad/legal**: HA/escala gateway/Janus/coturn, coste TURN,
    monitorización self-hosted, auditoría GOST/pentest/GPLv3, export/import.
15. **Pruebas/aceptación**: netem, dispositivos, cold start WSS/Telegram,
    TURN/cortafuegos, IPv6/CGNAT, Bluetooth/hotplug, handover, carga/soak.

## 16. Anexos

### 16.1 Presets
| Preset | Video | FPS | Audio | Uso |
|---|---|---|---|---|
| Auto | adaptativo | 15-30 | Opus adaptativo | default |
| Ahorro | 360p o off | 15 | 16 kbps | batería/red mala |
| Balanceado | 360-540p | 24 | 24 kbps | uso general |
| HD | 720p | 30 | 32 kbps | WiFi/carga |
| Solo audio | off | — | 24-32 kbps | audio, pantalla off |
| Walkie | off | — | 6-16 kbps | PTT, red deficiente |

### 16.2 Checklist de seguridad
- [ ] E2EE obligatoria, sin fallback.
- [ ] PFS y rotación de claves.
- [ ] Verificación de claves (safety number/QR/epoch).
- [ ] SFU sin claves, sin grabación, sin transcodificación.
- [ ] GOST TLS o TLS 1.3, HSTS, CA privada + pinning (Android/Qt/Windows), pins de respaldo.
- [ ] Tokens de llamada/ring/floor de un solo uso y TTL corto.
- [ ] Autorización por contacto/bloqueo; rate limits.
- [ ] TURN con credenciales temporales, cuotas y rangos denegados.
- [ ] Claves GOST cifradas con Kuznyechik + PBKDF2-Streebog (opcional Keystore/DPAPI/TPM); logs sin secretos ni contenido.
- [ ] Metadatos minimizados, cifrados en reposo, retención definida.
- [ ] WSS persistente + Telegram deep link; sin FCM/APNs/WNS.
- [ ] Gateway C valida GOST y acuña token Janus; Janus no expuesto.
- [ ] DTLS-SRTP estándar aceptado como tubería; contenido E2EE GOST.
- [ ] APK/MSI/AppImage firmados; update self-hosted; sin Play Store.
- [ ] Pentest + fuzz + revisión de dependencias.

### 16.3 Alcance GOST (real vs. objetivo)

| Capa | GOST disponible | Estado |
|---|---|---|
| Hash | Streebog-256/512 (RFC 6986 streaming) | Sí; usar streaming, no el one-shot reverso |
| HMAC/KDF | HMAC-Streebog, PBKDF2, HKDF | Sí |
| Firma | GOST R 34.10-2012 | Sí |
| Acuerdo de claves | VKO R 50.1.113-2016 | Sí |
| Curvas | 2001 y TC26 2012 (256/512) | Sí |
| Cifrado simétrico | Magma (GOST 28147-89) gamma/CFB | Sí (fallback) |
| Kuznyechik (GOST R 34.12-2015) | Sí (`b436423`) | Implementado con vectores |
| AEAD/CMAC | Sí (`b436423`) | Kuznyechik-CTR-ACPKM + CMAC (EtM) |
| SFrame-GOST | Sí (`b436423`) | Perfil propio; replay/KID |
| AES/ECDHE/Ed25519 | — | **No** para contenido/identidad |
| RNG | `getentropy`/`BCryptGenRandom` | Sí (revisar Android/Linux/Windows) |
| Constant-time | — | **Pendiente**; S-box con cache-timing; auditar |
| TLS | — | **GOST TLS** objetivo (OpenSSL+gost-engine); fallback TLS 1.3 + pinning |
| DTLS | — | **No práctico**; DTLS-SRTP estándar como tubería |
| TURN/Janus tokens | — | Estándar HMAC; gateway C para GOST en el borde |
| Push | — | WSS persistente + Telegram deep link; sin FCM/APNs/WNS |

### 16.4 Terceros (con las decisiones actuales)

| Necesidad | Tercero | Decisión / alternativa | Pendiente |
|---|---|---|---|
| Ring Android | — | **WSS persistente** + Telegram deep link | Doze/OEM/batería; sin ring si app cerrada |
| Ring Linux | — | **WSS persistente** (systemd user/tray) | Ninguno |
| Ring Windows | — | **WSS persistente** + tray | Ninguno |
| Notificación Telegram | Telegram | Aceptado **solo** para `call_id`/deep link | Sin media ni contenido |
| TLS apps | — | **CA privada + pinning**; **GOST TLS** objetivo | Jerarquía, rotación, revocación, pins |
| App stores | — | **No**: APK sideload, MSI/portable, AppImage/deb | Firma/actualización self-hosted |
| Code signing | CAs (opcional) | Clave propia/self-signed | SmartScreen/Gatekeeper |
| DNS/NTP | Proveedor DNS/NTP | DNS/NTP propio | Ninguno |
| Código/CI | GitHub/CI cloud | Git/CI self-hosted | Ya usan GitHub; aclarar |
| Janus | — | Self-hosted; **GPLv3** | Revisar obligaciones si se distribuye |

> Con estas decisiones **no hay FCM/APNs/WNS, app stores, CA pública ni web**.
> Las únicas dependencias externas son Telegram (notificación aceptada),
> DNS/NTP y, opcionalmente, firma de código. Janus es GPLv3.

### 16.5 Glosario
- **E2EE**: cifrado extremo a extremo; el servidor no puede descifrar.
- **SFU**: servidor que reenvía flujos sin mezclar.
- **PFS**: forward secrecy.
- **Sender keys**: clave por emisor para E2EE de grupo (sin MLS).
- **Kuznyechik**: cifrado de bloque GOST R 34.12-2015.
- **DTLS-SRTP**: transporte estándar de WebRTC; aquí solo tubería, contenido E2EE GOST.
- **Floor**: turno de habla en walkie.
- **R-Budget**: presupuesto de fiabilidad que reinvierte ahorros.
- **PTT**: push-to-talk.
