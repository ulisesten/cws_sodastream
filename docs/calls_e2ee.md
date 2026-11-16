# E2EE de media (SFrame-GOST) — implementación

> Estado: **fases A+B implementadas** (2026-11-16) en el cliente Android.
> Cifra frames de audio y vídeo con GOST. La autenticación de identidades y
> el ratchet quedan para la siguiente fase; ver “Límites”.

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
           info = "sframe-gost-v1",
           L    = 32)
```

- `KID = 1`. Ambos extremos usan la misma clave y el mismo `call_id`.

## 2. Formato de frame (SFrame-GOST)

Perfil propio (no interoperable con SFrame RFC 9605):

```
byte 0      versión = 0x01
byte 1      KID
bytes 2..9  contador de frame (big-endian 64 bits)
resto       ciphertext || CMAC (16 B)
```

- Cifrado: Kuznyechik-CTR-ACPKM + CMAC (GOST R 34.12/34.13-2015).
- La cabecera es datos asociados autenticados.
- Cada stream (audio y vídeo) tiene su **propio contexto**: contador y
  ventana de replay de 64 frames.

## 3. Integración en WebRTC

- `c_gost_encryption` se compila con el NDK en `libgostsframe.so`
  (`app/src/main/cpp`).
- `GostE2ee` (JNI) expone keypair/VKO/HKDF y los contextos SFrame.
- `WebRtcEngine` implementa `FrameEncryptorInterface` y
  `FrameDecryptorInterface` (WebRTC m125) y los instala en cada
  `RtpSender`/`RtpReceiver`.
- `CallViewModel` añade/parsea el atributo SDP y deriva la clave.

## 4. Fail-closed

- Los tracks de audio y vídeo arrancan **deshabilitados**.
- Se habilitan solo cuando la clave está derivada y los encryptors están
  instalados.
- Si el SDP remoto no trae `a=x-gost-e2ee`, la llamada termina con
  “Sin cifrado E2EE”. No hay fallback a media en claro.

## 5. Límites actuales

- **Sin autenticación de la clave efímera**: el servidor de señalización
  (o un MITM de TLS) podría sustituir la clave. Falta X3DH-GOST con prekeys
  firmadas y safety numbers/QR.
- **Sin ratchet por mensaje**: la clave es por llamada; no hay
  post-compromise security.
- **Sin auditoría externa** de SFrame-GOST/VKO.
- **Sin SFU/grupo**: 1:1 P2P/TURN únicamente.
- El atributo SDP no está firmado; la autenticación llega con la fase de
  identidad.
