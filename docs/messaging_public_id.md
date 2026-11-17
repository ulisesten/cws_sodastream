# IDs públicos de mensajería duplicados (bug y fix)

Actualizado: 2026-11-16.

## Síntoma

- El inbox muestra el mismo contacto dos veces (p. ej. "OpenCode" y
  "OpenCode · Nueva conversación", o "ChatB" duplicado).
- Al abrir la entrada duplicada, el chat falla o queda vacío.
- En el dispositivo aparecen conversaciones con IDs distintos para la misma
  persona:
  - `ERzg_O6TLYPN` y `Mw0RjXCObfNA` (OpenCode).
  - `EWc3VGjLzS-o` y `tejRZUY7HpSu` (ChatB).
- En el Pixel 3 había dos cachés de inbox con claves de **usuario propio**
  distintas: `conversations_Mw0RjXCObfNA` y `conversations_tejRZUY7HpSu`.

## Causa raíz

`POST /api/v1/messaging/setup` generaba un **nanoid nuevo en cada llamada** y
sobrescribía `cat_usuarios.usu_id_public`:

```c
char* public_id = jwt_core_nanoid(USU_ID_PUBLIC_LEN);
sql_result_t* dao = users_domain_set_public_id(u->usu_id, public_id);
```

El cliente llama a `setup` cuando no tiene `messagingPublicId` guardado
(instalación nueva, datos borrados, cambio de sesión). Cada setup rotaba el
id, de modo que:

1. El mismo usuario aparecía con ids viejos y nuevos.
2. Los contactos guardaban varios ids para la misma persona.
3. Los ids viejos dejaban de resolver → error al abrir el chat duplicado.
4. El historial quedaba partido entre ids.

## Fix servidor

`POST /messaging/setup` ahora es **idempotente**: si el usuario ya tiene
`usu_id_public`, lo devuelve; solo genera uno si no existe.

Cambios:

- `sql/procUsersCons.sql`: nuevo `@USU_BY_ID_CONS = 5` que devuelve
  `usu_id, usu_id_public` por `usu_id`.
- `src/api/v1/routes/users/domain/users_domain.{h,c}`:
  `users_domain_get_public_id(usu_id)` (`CONS_USU_BY_ID = 5`).
- `src/api/v1/routes/messaging/messaging_routes.c`:
  `messaging_setup_handler` consulta el id actual y reutiliza; solo genera y
  guarda si no hay.

> **Despliegue:** aplicar `sql/procUsersCons.sql` (ALTER PROCEDURE) **antes**
> de desplegar el binario. Si la BD conserva la versión vieja del SP, la
> consulta tipo 5 devuelve 0 filas y el handler volvería a generar un id
> nuevo (comportamiento anterior). Verificar con:
>
> ```sql
> EXEC procUsersCons @tipoConsulta = 5, @usu_id = <id>;
> ```

## Fix cliente (mitigación inmediata)

Aunque el servidor tarde en desplegarse, el cliente ya no muestra duplicados:

- `MessagingViewModel.canonicalPublicId(id, nombre)`: si el id no está en la
  lista actual de usuarios, busca un **único** usuario con el mismo nombre y
  usa su id. Si hay homónimos, no fusiona.
- `refreshInbox()`: colapsa la caché por id canónico, conserva el mensaje más
  reciente (`lastMsgId`/timestamp), toma el máximo de no leídos y guarda la
  caché limpia.
- `openConversation()`: resuelve el id viejo al canónico antes de pedir
  historial; ya no abre ids muertos.
- `handleRealtimeMessage()`: los `dm` con `from` viejo se asocian al contacto
  canónico, tanto para el inbox como para el chat abierto.
- `upsertConversation()`: guarda siempre bajo el id canónico.

## Limitaciones / pendiente

- El historial ya partido entre ids viejos **no se fusiona** en la BD. Para
  reconstruirlo hace falta una migración de datos: mapear `usu_id_public`
  viejo → actual (por nombre/usuario) y actualizar las conversaciones y
  mensajes. Hacerlo con backup y validación.
- Si dos usuarios distintos comparten nombre, el cliente no los fusiona (no
  hay forma segura de distinguirlos sin el id estable). Con el fix del
  servidor, el id ya es estable.
- Añadir un endpoint `GET /messaging/me` o incluir `usu_id_public` en la
  respuesta de login para que el cliente no dependa de `setup` tras una
  reinstalación.
- Test de regresión: llamar `setup` dos veces y comprobar que devuelve el
  mismo `usu_id_public`; crear dos conversaciones y verificar que no aparecen
  duplicados en `/messaging/users`.
