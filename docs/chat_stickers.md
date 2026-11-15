# Chat 1:1 — Stickers locales (spec de implementación)

Especificación para que el agente implemente **stickers en el chat 1:1** en
`cws_sodastream`. El cliente mantiene un catálogo **local** de stickers
(importados de una carpeta/`.wastickers` propios de la app), cada uno con un
**id estable por contenido**, y el servidor actúa como **registro/entrega** para
que el receptor pueda obtener el archivo cuando no lo tenga.

> Idea central: el mensaje **no lleva el archivo**, lleva `tipo=sticker` y
> `sticker_id` (hash). El receptor busca el id en su catálogo local; si no lo
> tiene, lo descarga una vez desde el servidor y lo cachea. El emisor solo sube
> el archivo al registro si aún no existe.

---

## 0. Estado de implementación (servidor)

- Endpoints implementados en `cws_sodastream` (`/api/v1/messaging/stickers...`).
- SQL en `db_sodastream/` (aplicar): `propuesta_chat_stickers.sql`,
  `procChatStickersProc.sql`, `procChatStickersCons.sql` y las versiones
  actualizadas de `procChatMensajesProc/Cons.sql`.
- `STICKERS_DIR` agregado al `.env` como ruta **absoluta** (igual que
  `HLS_DIR`/`API_NAS`); el API crea el directorio al iniciar y lo revalida
  antes de cada upload.
- `opr_chat_stickers`: PK `sti_id INT IDENTITY`; el hash de contenido va en
  `sti_hash CHAR(64) UNIQUE` (id público de la API).
- Campo extra `sti_id_created_by INT` (usuario que registró el sticker;
  interno, no se expone).
- Ajustes al esquema aplicado: `msg_tipo` es `TINYINT` (`0`=texto,
  `1`=sticker; la API usa `"text"`/`"sticker"`) y `msg_texto` es `NOT NULL`
  (para stickers se guarda `''`).
- `404` al enviar un sticker no registrado (el cliente debe subirlo antes).

### Hallazgo de verificación (2026-11-15)

- **Hash real del `sticker_id`**: el handler usa `gost_hash` de
  `c_gost_encryption`, que es la variante
  `reverse(Streebog256(reverse(bytes)))` (ver `gost/hash/hash.h`), **no** el
  Streebog-256 estándar. Verificado: enviando ese id en `X-Sticker-Id` la
  validación pasa (el estándar y SHA-256 dan “X-Sticker-Id no coincide”). El
  cliente debe replicar esa variante para el id local, o usar el `sticker_id`
  que devuelve el upload.
- **502/upload >8 KB (resuelto)**: el server `cws` usaba un buffer fijo de
  recepción (`CWS_RECV_BUF=8192`) y parseaba requests incompletas; un body
  mayor cortaba la conexión (502). Ahora `cws_conn_t` usa un buffer dinámico
  que crece hasta `max_header+max_body`, espera la request completa antes de
  parsear y libera/encoge el buffer. También se corrigió `Connection: close`
  (se comparaba un valor de header no NUL-terminado). Commit `cws@3ab262a`.
- **Directorio resuelto en código**: el API crea `STICKERS_DIR` al iniciar
  (`mkdir -p`) y lo **revalida antes de cada upload**; si no existe o no es
  escribible prueba `STICKERS_DIR_RELATIVE` y, si tampoco, responde **500
  JSON** con la ruta y lo registra en el log. Los fallos de escritura/DB también
  son JSON y quedan logueados.
  - Requisito: que el **usuario del proceso** (p. ej. `ulisesten`) pueda
    crear/escribir en la ruta; el API no eleva permisos.
  - SQL: aplicar `propuesta_chat_stickers.sql` (incluye drop/recreate de
    tablas viejas) y `procChatStickersProc.sql` (fix `[exists]`).

---

## 1. Alcance

Incluye:

- Registro de archivos de sticker en el servidor (content-addressed).
- Descarga autenticada por id con caché inmutable.
- Inventario best-effort por usuario (qué stickers tiene localmente).
- Nuevo tipo de mensaje `sticker` en historial, envío y push `dm`.
- Flujo cliente: catálogo local, importación, envío, recepción y validación.

No incluye (futuro):

- Publicar packs en el catálogo/buscador de WhatsApp (requiere ContentProvider de
  stickers de WhatsApp y whitelisting con Meta).
- Edición/creación de stickers dentro de la app.
- Packs compartidos en el servidor (se puede añadir después; el MVP usa id por
  sticker).

### Limitación importante (WhatsApp)

WhatsApp guarda sus packs en almacenamiento privado y **no** hay API pública para
leerlos. Los “folders custom” deben ser **de la app**:

- carpeta propia (p. ej. `filesDir/sticker_packs/<pack>/` y
  `Android/data/<pkg>/files/Stickers/`), o
- importación por el share sheet de archivos `.wastickers` (zip con
  `manifest.json` + `.webp`) o de imágenes `.webp`.

Lo que sí se puede hacer (opcional, otro ticket) es **exponer** los packs de
esta app a WhatsApp con un `ContentProvider` de stickers; eso es un flujo
distinto al de este spec.

---

## 2. Decisiones de diseño

| Tema | Decisión |
|---|---|
| Id del sticker | `sticker_id` = **Streebog-256 hex (64 chars)** de los bytes exactos del archivo `.webp`. Content-addressed: mismo archivo ⇒ mismo id en todos los usuarios. |
| Almacenamiento servidor | Archivo en `STICKERS_DIR` con nombre `<sti_hash>.webp` + fila en `opr_chat_stickers`. |
| Transporte | El mensaje referencia `sticker_id`; el receptor descarga el archivo solo si no lo tiene. |
| Tipo de mensaje | `msg_tipo`: `text` (default) o `sticker`; el contenido del sticker es `sticker_id`. |
| Inventario | `opr_usuarios_sticker` (best-effort). Correctitud no depende de él: el receptor siempre valida localmente y descarga si falta. |
| Caché | `GET .../file` con `Cache-Control: immutable` y `ETag`; el cliente guarda el archivo y lo indexa por id. |
| Compatibilidad | Mensajes existentes sin `tipo` se tratan como `text`; `sticker_id` nulo. |

> Streebog-256 se elige por consistencia con el stack GOST. El endpoint de
> subida devuelve el id canónico, así un cliente que no quiera calcularlo puede
> usar el que devuelve el server. Para uso local/offline el cliente puede
> calcularlo con BouncyCastle (`GOST3411_2012_256Digest`, ya usado en la app).

---

## 3. Modelo de datos (SQL Server)

**Se reutiliza `opr_chat_mensajes.msg_tipo`, que ya existe y es `TINYINT`**
(`0` = texto, `1` = sticker). No se crea una columna `VARCHAR` nueva.

```sql
-- Registro de stickers (archivo en STICKERS_DIR/<sti_hash>.webp)
CREATE TABLE opr_chat_stickers (
    sti_id            INT IDENTITY(1,1) NOT NULL PRIMARY KEY,
    sti_hash          CHAR(64)    NOT NULL UNIQUE, -- Streebog-256 hex (id público)
    sti_mime          VARCHAR(32) NOT NULL DEFAULT 'image/webp',
    sti_bytes         INT         NOT NULL,
    sti_width         INT         NULL,
    sti_height        INT         NULL,
    sti_animated      BIT         NOT NULL DEFAULT 0,
    sti_id_created_by INT         NOT NULL, -- usuario que lo registró (interno)
    sti_created       DATETIME2   NOT NULL DEFAULT SYSUTCDATETIME()
);

-- Inventario local reportado por cada usuario (best-effort)
-- Cada tabla tiene PK int identity; los id: prefijo_id_entidad.
CREATE TABLE opr_usuarios_sticker (
    uss_id         INT IDENTITY(1,1) NOT NULL PRIMARY KEY,
    uss_id_usuario INT         NOT NULL,
    uss_id_sticker CHAR(64)    NOT NULL, -- sti_hash (contenido)
    uss_fecha_alta DATETIME2   NOT NULL DEFAULT SYSUTCDATETIME(),
    UNIQUE (uss_id_usuario, uss_id_sticker)
);
CREATE INDEX ix_uss_sticker ON opr_usuarios_sticker (uss_id_sticker);

-- Mensajes: solo falta el id del sticker (msg_tipo YA existe, TINYINT)
ALTER TABLE opr_chat_mensajes ADD msg_sticker_id CHAR(64) NULL;
```

Reglas del mensaje (BD):

- `msg_tipo = 0` (texto) ⇒ `msg_sticker_id` NULL y `msg_texto` requerido.
- `msg_tipo = 1` (sticker) ⇒ `msg_sticker_id` requerido (debe existir en
  `opr_chat_stickers`) y `msg_texto = ''` (la columna es `NOT NULL`).

### Stored procedures

`procChatMensajesProc` (insertar):

- Parámetros: `@tipo TINYINT = 0` (0 texto, 1 sticker),
  `@sticker_id CHAR(64) = NULL`.
- Validar `@tipo IN (0,1)`; si `1`, que exista
  `opr_chat_stickers.sti_hash = @sticker_id`; si no → error de validación.
- Insertar `msg_tipo = @tipo`, `msg_sticker_id` y `msg_texto` (`''` en sticker).
- La **API** sigue usando `"tipo":"text"|"sticker"`; el servidor lo mapea a
  `0/1` antes de llamar al SP.

`procChatMensajesCons` (historial):

- Devuelve `msg_tipo` (TINYINT) y `msg_sticker_id` además de los campos
  actuales (y metadatos del sticker si aplica).

---

## 4. Endpoints (Bearer)

Todos bajo `/api/v1/messaging`, requieren `Authorization: Bearer <access_token>`
y respetan el refresh/401 estándar (ver `AUTH_README.md`).

### 4.1 Registrar sticker

```http
POST /api/v1/messaging/stickers
Authorization: Bearer <access>
Content-Type: image/webp

<bytes WebP>
```

- El servidor **calcula** Streebog-256 del body, valida el formato y guarda.
- Idempotente por contenido: si ya existe, responde 200 con `exists:true`.
- Acepta también `X-Sticker-Id: <hex>` y verifica que coincida con el hash
  calculado (400 si no).

**201 (nuevo) / 200 (existente)**
```json
{"success":true,"error":0,"msg":"Sticker registrado",
 "data":{"sticker_id":"<64 hex>","mime":"image/webp","bytes":12345,
         "width":512,"height":512,"animated":false,"exists":false,
         "url":"/api/v1/messaging/stickers/<64 hex>/file"}}
```

Errores: `400` WebP inválido / dimensiones inválidas · `413` demasiado grande ·
`415` mime no soportado · `401` sin token · `500` fallo de almacenamiento.

Límites (validar en servidor):

- Máx. **1 MiB** por archivo (WhatsApp: estático ≤100 KB, animado ≤500 KB).
- Dimensiones **512×512** (rechazar si difiere; opcionalmente permitir y
  reescalar no recomendado).
- WebP estático o animado (`VP8X`/`ANIM`), sin otros formatos.
- Guardar el archivo con nombre `<sti_hash>.webp`; nunca usar nombres del cliente.

### 4.2 Consultar metadatos

```http
GET /api/v1/messaging/stickers/{sticker_id}
```

**200**
```json
{"success":true,"error":0,"msg":"Sticker encontrado",
 "data":{"sticker_id":"<64 hex>","mime":"image/webp","bytes":12345,
         "width":512,"height":512,"animated":false,
         "url":"/api/v1/messaging/stickers/<64 hex>/file"}}
```

`400` id con formato inválido · `404` no existe · `401`.

### 4.3 Descargar archivo

```http
GET /api/v1/messaging/stickers/{sticker_id}/file
Authorization: Bearer <access>
```

**200** `Content-Type: image/webp`
`Cache-Control: private, max-age=31536000, immutable`
`ETag: "<sticker_id>"`

- Lee `STICKERS_DIR/<sticker_id>.webp` (anti-traversal: el id es hex de 64).
- `404` si no existe · `401`.

### 4.4 Verificar disponibilidad (emisor)

```http
POST /api/v1/messaging/stickers/check
Content-Type: application/json

{"contact_public_id":"<contact_id>","sticker_ids":["<id>","<id>"]}
```

**200**
```json
{"success":true,"error":0,"msg":"Stickers verificados",
 "data":{"registered":["<id>"],"missing":["<id>"],
         "contact_has":["<id>"],"contact_missing":["<id>"]}}
```

- `registered`: existe en el registro del servidor (archivo disponible).
- `missing`: no existe en el registro → el emisor debe subirlo antes de enviar.
- `contact_has` / `contact_missing`: según el inventario del contacto
  (`opr_usuarios_sticker`). Si el contacto nunca sincronizó, se considera
  `contact_missing` (seguro).
- `contact_public_id` es opcional; sin él solo devuelve `registered`/`missing`.

### 4.5 Sincronizar inventario local

```http
POST /api/v1/messaging/stickers/sync
Content-Type: application/json

{"sticker_ids":["<id>","<id>"]}
```

**200** `{"success":true,"error":0,"msg":"Inventario sincronizado","data":{"synced":2}}`

- Reemplaza el inventario del usuario autenticado por la lista enviada (o hace
  merge; definir: **reemplazo** para reflejar exactamente lo local).
- Validar ids hex de 64; ignorar duplicados. No exige que existan en el
  registro (el cliente puede tener stickers aún no subidos).
- Llamar al arrancar y tras importar/descargar stickers (best-effort).

---

## 5. Cambios en mensajes

### 5.1 Enviar

`POST /api/v1/messaging/conversations/{usu_id_public}/messages`

Texto (sin cambios, `tipo` por defecto):
```json
{"texto":"hola","reply_to":123}
```

Sticker:
```json
{"tipo":"sticker","sticker_id":"<64 hex>","reply_to":null}
```

- `tipo` ∈ `text` | `sticker`.
- Para `sticker`: `sticker_id` obligatorio y **debe existir** en
  `opr_chat_stickers` (el cliente subió el archivo antes con 4.1).
  `texto` debe venir vacío/ausente.
- `reply_to` aplica igual (mismo mensaje citado, ver `chat_1a1_cliente.md`).
- La **API** usa `"tipo":"text"|"sticker"`; en BD es `msg_tipo TINYINT`
  (`0`/`1`, ver §3).
- Errores: `400` tipo/parámetros inválidos · `404` sticker no registrado ·
  `404` contacto no existe · `401`.

**201**
```json
{"success":true,"error":0,"msg":"Mensaje enviado",
 "data":{"msg_id":130,"from":"<mi_id>","tipo":"sticker",
         "sticker_id":"<64 hex>","texto":"","fecha":"2026-11-15T10:00:00",
         "reply_to":null,"leido":false,
         "sticker":{"sticker_id":"<64 hex>","mime":"image/webp",
                    "width":512,"height":512,"animated":false,
                    "url":"/api/v1/messaging/stickers/<64 hex>/file"}}}
```

### 5.2 Historial y `dm`

Añadir a cada mensaje:

```json
{"msg_id":130,"from":"<id>","es_mio":false,"tipo":"sticker",
 "sticker_id":"<64 hex>","texto":"","fecha":"...","reply_to":null,
 "leido":false}
```

- En `dm` (push WS) incluir `tipo` y `sticker_id`; `from_nombre` sigue igual.
- Opcional (recomendado) incluir `sticker` con metadatos/url para ahorrar una
  llamada; si se omite, el cliente consulta 4.2/4.3 cuando no tenga el id.
- Mensajes de texto: `"tipo":"text"`, `"sticker_id":null` (o ausente para
  compatibilidad; el cliente asume `text`).

---

## 6. Flujo del cliente (Android)

### 6.1 Catálogo local

```
filesDir/sticker_packs/<pack_id>/
    manifest.json          # nombre, publisher, tray, orden, animated
    <sticker_id>.webp
filesDir/sticker_packs/received/
    <sticker_id>.webp      # descargados de otros usuarios
```

- Cada archivo se indexa por su `sticker_id` (Streebog-256 hex del archivo).
- Importar:
  - `.wastickers` (zip): leer `manifest.json` + `.webp`; copiar al store y
    calcular ids.
  - carpeta propia (`Android/data/<pkg>/files/Stickers` o `Documents/Stickers`):
    escanear `.webp` al arrancar.
  - share sheet: `ACTION_SEND`/`ACTION_SEND_MULTIPLE` con `image/webp` o el zip.
- Mostrar packs en un picker (tabs/grid); tray 96×96.

### 6.2 Enviar un sticker

1. Obtener el archivo local y su `sticker_id` (ya calculado al importar).
2. `POST /stickers/check` con `{contact_public_id, sticker_ids:[id]}`:
   - `registered=false` ⇒ `POST /stickers` (subir el WebP) y usar el id devuelto
     (verificar que coincide).
3. `POST /conversations/{contact}/messages` con
   `{tipo:"sticker", sticker_id, reply_to?}`.
4. UI optimista: render desde el archivo local; reemplazar por el `msg_id` del
   201 y deduplicar (el `dm` al emisor puede repetir; ver dedupe por `msg_id`).

> “Si el usuario no cuenta con el sticker, se envía el file”: el archivo se
> sube al registro (si no estaba) y el receptor lo descarga al no tenerlo. No se
> inlinea en el mensaje/WS.

### 6.3 Recibir un sticker

1. En historial o `dm` con `tipo=sticker`:
   - Si el `sticker_id` está en el índice local ⇒ render local.
   - Si no ⇒ `GET /stickers/{id}/file`, guardar en `received/<id>.webp`,
     indexar y `POST /stickers/sync` (best-effort), luego render.
   - Si falla la descarga ⇒ placeholder + reintento bajo demanda.
2. Render: sin burbuja, ~160 dp, `ContentScale.Fit`, fondo transparente.
   - WebP animado: Coil + `coil-gif` (API 28+ con `ImageDecoder`; en APIs
     menores, fallback estático o `android-gif-drawable`).
3. Al abrir el chat, no descargar todos: resolver por mensaje visible (o lazy).

### 6.4 Inventario

- Tras importar/descargar: `POST /stickers/sync` con todos los ids locales.
- Al arrancar: sincronizar (best-effort).
- El inventario es una optimización para el emisor (`contact_has`); la
  correctitud la garantiza el chequeo local + descarga del receptor.

---

## 7. Seguridad y validación

- Auth Bearer obligatoria en todos los endpoints.
- Validar `sticker_id` con regex `^[0-9a-f]{64}$` y usar solo ese nombre para el
  archivo (anti path traversal).
- Validar WebP real (magic `RIFF....WEBP`), dimensiones 512×512, tamaño ≤1 MiB.
- Rate limit de subida (p. ej. 30/min) y de check/sync.
- No confiar en mime/nombre del cliente; el servidor fija `image/webp`.
- `Content-Security`/descarga: nunca ejecutar; servir como `image/webp`.
- `ETag` y caché inmutable por id; borrar archivos huérfanos solo con job
  administrativo (no borrar si hay mensajes que los referencian).

---

## 8. Compatibilidad y migración

- `msg_tipo` DEFAULT `'text'` ⇒ mensajes existentes sin cambios.
- Cliente viejo: ignora `tipo`/`sticker_id`; un mensaje sticker se vería vacío.
  Para evitarlo, el servidor puede enviar `texto` con un fallback tipo
  `"[sticker]"` a clientes viejos (opcional; o el cliente viejo muestra
  placeholder si `texto` vacío).
- La subida es idempotente; no hace falta migrar stickers existentes.
- `STICKERS_DIR` nuevo en `.env`; crear el directorio al iniciar.

---

## 9. Checklist de implementación

### Servidor

- [ ] `STICKERS_DIR` en `.env` + crear directorio.
- [ ] Tablas `opr_chat_stickers`, `opr_usuarios_sticker` y columnas
      `msg_tipo`/`msg_sticker_id`.
- [ ] `POST /stickers` (validar WebP, hash Streebog, guardar, idempotente).
- [ ] `GET /stickers/{id}` y `GET /stickers/{id}/file` (ETag/caché).
- [ ] `POST /stickers/check` y `POST /stickers/sync`.
- [ ] Mensajes: parsear `tipo`/`sticker_id`; validar existencia en el registro.
- [ ] SPs `procChatMensajesProc`/`Cons` con los campos nuevos.
- [ ] Respuestas de historial y push `dm` con `tipo`/`sticker_id`.
- [ ] Tests: upload/dedupe, límites, 401/404, mensaje sticker, historial, dm,
      inventario/check, path traversal.

### Cliente (Android)

- [ ] Store local + import (`.wastickers`/carpeta/share), índice por id
      (Streebog con BouncyCastle).
- [ ] Repositorio: upload/check/sync/file (Volley, sin caché para los JSON;
      archivo con cache local).
- [ ] Modelo/parseo de mensajes `tipo`/`sticker_id`; historial y `dm`.
- [ ] UI: picker de packs; render sin burbuja (animado con `coil-gif`).
- [ ] Envío: check → upload si falta → POST mensaje; optimista + dedupe.
- [ ] Recepción: local o descarga a `received/`; sync de inventario.
- [ ] Placeholder/errores si no hay archivo/red.

---

## 10. Tests sugeridos

1. Subir el mismo WebP dos veces ⇒ mismo `sticker_id`, `exists=true`.
2. WebP >1 MiB / dimensiones ≠512 / no-WebP ⇒ 400/413/415.
3. Enviar sticker no registrado ⇒ 404; tras subirlo ⇒ 201.
4. Historial y `dm` devuelven `tipo=sticker` y `sticker_id`.
5. Receptor sin sticker ⇒ descarga y render; con sticker ⇒ no descarga (verificar
   que no se pide el archivo).
6. `sync` + `check` con contacto: `contact_has`/`contact_missing` coherentes.
7. `reply_to` funciona con stickers (cita un sticker y viceversa).
8. Ids inválidos/path traversal ⇒ 400/404.
9. Cliente viejo no rompe con `tipo` desconocido.

---

## 11. Preguntas abiertas

- ¿El id debe ser **Streebog-256** (consistente con el stack) o **SHA-256**
  (más universal para futuros clientes web)? Se puede aceptar el `X-Sticker-Id`
  del cliente y verificar; el server devuelve el canónico.
- ¿Sincronización de inventario por **reemplazo** (recomendado) o merge?
- ¿Permitir packs compartidos en el servidor (tray + nombre) ahora o después?
  El MVP solo direcciona stickers individuales.
- ¿Límite de tamaño 1 MiB o ajustar a la spec de WhatsApp (100/500 KB)?
- Retención/limpieza de stickers sin mensajes (job administrativo).
