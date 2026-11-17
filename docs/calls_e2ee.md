# E2EE de media (FrameCryptor-GOST) — implementación

> Estado: **audio y vídeo cifrados con GOST** en el cliente Android
> (2026-11-16). La clave es efímera por llamada (VKO/HKDF). La autenticación
> de identidades y el ratchet quedan para la siguiente fase; ver “Límites”.

## 1. Clave efímera por llamada

Cada extremo genera un par de claves **GOST R 34.10-2012** (curva
`TC26-256-A`) al iniciar o recibir la llamada:

- La clave pública se serializa como `x || y` (32 + 32 bytes, big-endian) y
  viaja en el SDP como atributo de sesión:

```
a=x-gost-e2ee:v1:<base64(x||y)>
```

- Al recibir el SDP del par se valida el punto (en la curva, no infinito y
  en el subgrupo de orden `q`) y se deriva la clave:

```
shared = VKO(priv, peer_pub, UKM=1)          # R 50.1.113-2016
key    = HKDF-Streebog-256(
           salt = call_id,
           ikm  = shared,
           info = "gost-call-v1",
           L    = 32)
```

- Ambos extremos usan la misma clave de 32 B y el mismo `call_id`.

## 2. Cifrado de frames (FrameCryptor del fork)

El media se cifra dentro del `FrameTransformer` de `FrameCryptor`
(`api/crypto/frame_crypto_transformer.cc`) con un algoritmo **GOST** añadido
al fork de WebRTC. Este es el camino que maneja correctamente la
fragmentación y el marcador RTP de vídeo.

Layout de un frame cifrado:

```
cabecera de códec (claro) || ciphertext || CMAC(16) || IV(16) || len(IV) || key_index
```

- Cifrado: **Kuznyechik-CTR-ACPKM + CMAC** (GOST R 34.12/34.13-2015),
  encrypt-then-MAC, tag de 16 B.
- La cabecera de códec (VP8/H.264/AV1) es AAD autenticado y va en claro
  porque el depacketizer la necesita.
- `IV = SSRC || timestamp || (timestamp - contador) || contador`, único por
  frame. `len(IV) = 16`; `key_index = 0`.
- En H.264 el bloque cifrado se somete a *RBSP escaping* como el resto del
  payload.
- La clave que usa el AEAD la deriva `FrameCryptor` con
  `PBKDF2-HMAC-SHA256(salt fija de 16 B, 100000 iteraciones, 256 bits)` a
  partir del material VKO/HKDF. La sal es idéntica en ambos extremos; el
  ratchet está desactivado en esta fase.

## 3. Integración en el cliente

- Fork: `webrtc-sdk/webrtc`, rama `m125_release` (base del AAR
  `125.6422.07`), en `Development/webrtc-gost-build/src`.
  - `third_party/c_gost_encryption`: AEAD/HKDF vendorizados.
  - `FrameCryptorAlgorithm.GOST` (ordinal 1) → `Algorithm::kGost`.
  - `FrameCryptorTransformer`: IV de 16 B y AEAD GOST.
  - Detalles de build en `E2EE_GOST.md`.
- El AAR resultante se copia a `mobile/app/libs/libwebrtc-gost.aar`
  (arm64-v8a) y sustituye a la dependencia de Maven.
- `WebRtcEngine` crea un `FrameCryptorKeyProvider` compartido y un
  `FrameCryptor` GOST por `RtpSender`/`RtpReceiver`:
  - `sharedKey = true`, `ratchetWindowSize = 0`, `keyRingSize = 16`,
    `failureTolerance = -1`,
    `discardFrameWhenCryptorNotReady = true`.
  - `setSharedKey(0, key)` con la clave VKO/HKDF de 32 B.
- `CallViewModel` añade/parsea el atributo SDP, deriva la clave y llama a
  `engine.setE2eeKey()` antes de decodificar media.
  - Valida versión `v1`, que el atributo aparezca una sola vez y que la clave
    pública sea de 64 B; una versión desconocida termina la llamada.
  - Rechaza un cambio de clave dentro de la misma llamada (rekey no soportado).
- La pantalla de llamada muestra `E2EE GOST · <código>`: Streebog-256 de la
  clave derivada (4 grupos de 4 hex) para que ambos usuarios lo comparen por
  voz. Es una verificación manual, no sustituye a la identidad autenticada.
- Los `FrameCryptor` tienen un `Observer` que registra
  `DECRYPTIONFAILED`/`ENCRYPTIONFAILED`/`MISSINGKEY`/`INTERNALERROR` para
  diagnóstico.

## 4. Fail-closed

- Los tracks de audio y vídeo arrancan **deshabilitados**.
- Se habilitan solo cuando la clave está en el key provider y los
  `FrameCryptor` GOST están instalados.
- `discardFrameWhenCryptorNotReady = true`: si el cryptor no está listo, el
  frame se descarta; nunca se reenvía en claro.
- Si el SDP remoto no trae `a=x-gost-e2ee` (o falla la derivación), la
  llamada termina con “Sin cifrado E2EE”. No hay fallback a media en claro.

## 5. Límites actuales

- **Sin autenticación de la clave efímera**: el servidor de señalización
  (o un MITM de TLS) podría sustituir la clave. Falta X3DH-GOST con prekeys
  firmadas y safety numbers/QR.
- **Sin confirmación automática de clave**: el código corto es manual. Falta
  el MAC cruzado por DataChannel o por la API.
- **Sin ratchet por mensaje**: la clave es por llamada; no hay
  post-compromise security.
- **Sin auditoría externa** de la construcción AEAD, del fork ni de VKO.
- **Sin SFU/grupo**: 1:1 P2P/TURN únicamente.
- El atributo SDP no está firmado; la autenticación llega con la fase de
  identidad.
- El AAR del fork es arm64-v8a; falta build multi-ABI y firma para
  distribución.

Lista detallada y cambios necesarios en la API:
`docs/calls_e2ee_pending.md`.
