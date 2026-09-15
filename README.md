# cws_example_app

Reference consumer of the `cws` HTTP library via a git submodule.

## Layout

```
.
├── CMakeLists.txt
├── main.c
└── third_party/cws/      # git submodule (this repo's companion)
```

## Build & run

```bash
# 1. get sources + submodules
git clone --recurse-submodules <this-repo-url> cws_example_app
cd cws_example_app

# 2. configure + build
cmake -S . -B build
cmake --build build -j

# 3. run
./build/cws_example_app
```

The server listens on `:8181`. Try:

```bash
curl http://127.0.0.1:8181/
curl http://127.0.0.1:8181/healthz
curl http://127.0.0.1:8181/users/42
curl http://127.0.0.1:8181/metrics
```

## Updating the library

```bash
git submodule update --remote third_party/cws
git add third_party/cws
git commit -m "bump cws"
```

## How it works

`CMakeLists.txt` calls `add_subdirectory(third_party/cws)`, which registers a
`cws` static library target that exports include paths and compile flags
(`-march=x86-64-v3`, `-mtune=native`). The host target just links it:

```cmake
add_subdirectory(third_party/cws)
target_link_libraries(cws_example_app PRIVATE cws)
```

For an installed `cws` (instead of a submodule), swap the above lines for:

```cmake
find_package(cws REQUIRED CONFIG)
target_link_libraries(cws_example_app PRIVATE cws::cws)
```

---

## Pendientes de seguridad

Análisis de vulnerabilidades del módulo de autenticación (web + móvil).
Pendientes **por corregir**, ordenados por severidad.

### ✅ Corregido

- **#1 Firma del token sin cubrir el payload completo.** El `sign` autenticaba
  solo `id`+cola; `exp`, `session_type` y los campos de `user` quedaban sin
  integridad (CFB maleable). Ya se firma una cadena canónica con todos los
  campos — ver `sign_build()` en `core/jwt_core.c` (commit `066ed9f`).

### ⬜ Pendientes

| # | Sev. | Tema | Dónde |
|---|------|------|-------|
| 2 | Alta | Hash de contraseñas rápido y sin salt | `src/api/v1/routes/users/users_routes.c:222`, `core/jwt_core.c` (`jwt_core_verify_password`) |
| 3 | Alta | Sin rate limiting ni lockout en login | `core/authorization*.c`, `src/api/v1/routes/{users,mobile}` |
| 4 | Media | IV fijo derivado del secreto (cifrado determinista) | `core/jwt_core.c:431` |
| 5 | Media | JSON injection en respuestas móviles | `core/authorization_mobile.c:446`, `src/api/v1/routes/mobile/mobile_routes.c:32` |
| 6 | Media | Sin revocación/logout (`jti`/`aud`) | `core/jwt_core.{h,c}` |
| 7 | Media | CSRF no ligado a sesión; refresh acepta cualquier sufijo | `core/jwt.c` (`jwt_verify_csrf_token`), `core/authorization.c` |
| 8 | Media | `SECRET_KEY` reutilizada para IV + firma + CSRF | `core/jwt_core.c` |
| 9 | Baja | HTTP sin TLS; cookie `Secure` atada a `NODE_ENV` | `src/main.c`, `core/authorization.c` |
| 10 | Baja | Sin validación de formato de correo / fuerza de contraseña / verificación de email | `users_routes.c` (user_new), `authorization_mobile.c` |
| 11 | Baja | Enumeración por hash determinista de correo | `users_routes.c`, `core/authorization.c` |

Detalle y remediación sugerida:

- **#2 Contraseñas.** Se guarda `Streebog-256(password.trim())` (hash rápido,
  sin salt). `usu_salt` se genera pero **no participa** del hash.
  → Usar KDF (PBKDF2 ≥ 100k o Argon2/bcrypt) con el `usu_salt` por usuario.
- **#3 Rate limit.** Ningún límite en `/api/v1/users/signin` ni
  `/api/v1/auth/mobile/signin`; la lib trae `cws_mw_ratelimit_simple` sin usar.
  → Aplicar el middleware + backoff/bloqueo por cuenta.
- **#4 IV.** `cipher_iv` = primeros 8 bytes de `SECRET_KEY`, igual para todos
  los tokens → cifrado determinista (mismo prefijo ⇒ mismo ciphertext).
  → IV aleatorio por token, incluido en el token.
- **#5 Escape JSON.** `signin` móvil y `/me` insertan `usu_nombre`/`usu_correo`
  con `%s` sin escapar (el web sí usa `json_escape_to`).
  → Reutilizar el escape JSON en las respuestas móviles.
- **#6 Revocación.** Tokens válidos hasta `exp`, sin logout ni blacklist, sin
  `jti`. → Añadir `jti` + store de revocación (o versionado por usuario).
- **#7 CSRF.** `jwt_verify_csrf_token` acepta sufijo de sesión o
  `refresh_csrf`, y el CSRF no está atado al access token/usuario; el refresh
  no exige específicamente un `refresh_csrf`.
  → Ligar CSRF a `usu_id`/sesión y exigir el sufijo por endpoint.
- **#8 Separación de claves.** `SECRET_KEY` sirve de IV, firma de tokens y CSRF.
  → Derivar subclaves por uso (HKDF/etiqueta).
- **#9 TLS/cookies.** Con `NODE_ENV=production` sobre HTTP las cookies `Secure`
  las descarta el navegador; con `NODE_ENV` distinto viajan en claro.
  → Servir con TLS (`cws_app_tls`) y `Secure` siempre en producción.
- **#10 Entrada.** Sin validación de correo/fortaleza de contraseña ni
  verificación de email en el registro.
- **#11 Enumeración.** `usu_correo = Streebog(email)` es determinista; con
  lectura de BD se enumeran correos.

### Buenas prácticas ya presentes

- DAO por **stored procedures con parámetros ODBC tipados** (sin inyección SQL).
- **`session_type` distinto por canal** + colas de firma → un token móvil no
  vale en web ni viceversa.
- Comparaciones **timing-safe** (`jwt_safe_compare`) y errores de login
  uniformes (mismo mensaje para usuario inexistente y contraseña errónea).

