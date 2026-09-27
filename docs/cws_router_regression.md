# bug(cws/routing): 7e1a9d9 rompe sub-routers anidados y preflight CORS

> **Estado: RESUELTO.** La rama `devel` de `cws` fue reescrita (force-push) y la
> punta real `5721a38` ya restaura `method_serves()` y el offset `skipped`
> (`1adddb4 fix(router): offset de sub-routers anidados`). Verificado en
> `cws_sodastream`: `GET /api/v1/users/me` → 401 sin cookie / 200 con cookie, y
> `OPTIONS /api/v1/users/signin` → 204. Se subió el pin a `5721a38`.

## Resumen

La actualización del submódulo `cws` a **`7e1a9d9`**
(`35d26db feat(websocket): …` + `7e1a9d9 feat(middleware): cws_mw_cors_configure`),
aplicada en el commit `9b0d4a2 chore: actualiza submódulo cws a 7e1a9d9`, introdujo
**dos regresiones** en `third_party/cws/src/routing/router.c`:

1. **Sub-routers anidados no matchean → 404** (`/api/v1/users/me`,
   `/api/v1/auth/mobile/me`).
2. **Preflight CORS (OPTIONS) → 404** (antes lo respondía el middleware CORS).

No es un fallo de `cws_sodastream`: el mismo código de la app devolvía `200`/preflight
con el pin anterior `6125345`. El bug vive en la **librería `cws`**.

## Impacto

- El front (`streaming_server`) rompe: `fun.constants.js` llama a
  `/api/v1/users/me` para la sesión web, y el flujo con cookies en otro origen
  necesita el preflight `OPTIONS`. Ambos dan `404` → la sesión no se valida y el
  CORS con credenciales no funciona.
- Afecta a cualquier ruta montada **dentro** de otro router (anidada) y a todo
  preflight CORS.

## Evidencia

Probado en el build de `HEAD` (con los cambios de endpoints reintegrados y también
con ellos quitados por `git stash`; en ambos casos da 404):

| Petición | Pin `6125345` (antes) | `7e1a9d9` (ahora) |
|---|---|---|
| `GET /api/v1/users/me` | 200 | **404** |
| `GET /api/v1/auth/mobile/me` | 200 | **404** |
| `OPTIONS /api/v1/users/signin` (Origin + Access-Control-Request-Method) | 204 | **404** |
| `OPTIONS /api/v1/videos` | 204 | **404** |

## Causa — `src/routing/router.c`, diff `6125345..7e1a9d9`

### 1. Se elimina el offset `skipped` de los `/` iniciales

```diff
-        size_t skipped = 0;
-        while (rest_len > 0 && rest[0] == '/') { rest++; rest_len--; skipped++; }
+        while (rest_len > 0 && rest[0] == '/') { rest++; rest_len--; }
         ...
-        /* sub_win.consumed es relativo a `rest` (ya sin los '/' iniciales);
-         * hay que sumar también los separadores saltados para mantener el
-         * offset absoluto en `path` (necesario con sub-routers anidados). */
-        win.consumed += skipped + sub_win.consumed;
+        win.consumed += sub_win.consumed;
```

`/me` se monta **dentro** de `/api/v1/users` y `/api/v1/auth/mobile`
(`users_routes.c:306`, `mobile_routes.c:61` → `cws_router_mount(r, "/me", prot)`).
Al no sumar los separadores saltados, el offset absoluto queda corto y el
sub-router interno no matchea → 404.

### 2. Se elimina `method_serves()`

```diff
-/* ¿La ruta registrada sirve para `method`? OPTIONS (preflight CORS) se
- * considera servible por cualquier ruta para que el middleware CORS pueda
- * responder el preflight, aunque la ruta sea POST/GET/etc. */
-static int method_serves(cws_method_t reg, cws_method_t method) {
-    return reg == method || reg == CWS_M_UNKNOWN || method == CWS_M_OPTIONS;
-}
...
-            if (sub_root->handler && method_serves(sub_root->method, method)) {
+            if (sub_root->handler && (sub_root->method == method || sub_root->method == CWS_M_UNKNOWN)) {
```

Sin `method_serves`, una petición `OPTIONS` ya no encuentra ruta → 404 (el
middleware CORS nunca llega a ejecutarse). Aplica igual a rutas normales y a
sub-routers.

## Fix propuesto

Restaurar ambos hunks en `src/routing/router.c` (≈6 líneas):

- volver a acumular `skipped` y sumarlo a `win.consumed`;
- restaurar `method_serves()` y usar `method_serves(cur->method, method)` /
  `method_serves(sub_root->method, method)`.

> Nota: `method_serves` vs. el nuevo `CWS_M_UNKNOWN` — revisar si la intención del
> commit era mover el manejo de OPTIONS al servidor/middleware; si es así, hay que
> cubrir el preflight por otra vía y documentarlo. El caso del offset `skipped` es
> un bug directo sin contrapartida.

## Verificación

Tras el fix, en `cws_sodastream`:

```bash
curl -s -o /dev/null -w "%{http_code}\n" http://127.0.0.1:8080/api/v1/users/me
# 401 (sin cookie) en vez de 404  → la ruta ya matchea
curl -s -o /dev/null -w "%{http_code}\n" -X OPTIONS http://127.0.0.1:8080/api/v1/videos \
  -H "Origin: https://video.sodastream.fun" -H "Access-Control-Request-Method: GET"
# 204 (preflight)
```

## Relación con otros cambios

- Submódulo actualizado: `9b0d4a2 chore: actualiza submódulo cws a 7e1a9d9`
  (pin anterior: `6125345`).
- Descubierto al verificar los endpoints nuevos de `cws_sodastream`
  (thumbnails/views/relacionados/releases), que sí pasan; el fallo es ajeno a
  ellos.
