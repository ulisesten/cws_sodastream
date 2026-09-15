# cws_sodastream

Servidor HTTP en C para SodaStream: usa el framework [`cws`](#submódulos) como
submódulo, acceso a SQL Server por ODBC, autenticación web/móvil con tokens
GOST, servidor estático HLS y configuración central por `.env`.

## Layout

```
.
├── CMakeLists.txt
├── run.sh                     # instala deps, compila y ejecuta
├── core/                      # lógica central (independiente de las rutas)
│   ├── configuration.{h,c}    #   env central (cfg_getenv por hash)
│   ├── sql_eject.{h,c}        #   ejecutor de stored procedures (ODBC)
│   ├── jwt_core.{h,c}         #   primitivas de token compartidas web/móvil
│   ├── jwt.{h,c}              #   tokens web (cookies + CSRF)
│   ├── jwt_mobile.{h,c}       #   tokens móvil (Bearer, sin IP)
│   ├── authorization.{h,c}    #   middlewares web + signin
│   ├── authorization_mobile.{h,c} # middlewares/signin móvil
│   ├── password.{h,c}         #   PBKDF2-HMAC-Streebog + salt + pepper
│   └── hash_table.{h,c}       #   hash FNV-1a para cfg_getenv
├── src/                       # aplicación
│   ├── main.c
│   └── api/v1/routes/         #   videos, users, mobile
├── sql/                       # scripts SQL (SPs, tablas, migraciones)
├── docs/                      # documentación (p. ej. spec del KDF)
├── reference/                 # fuentes JS de referencia (origen del port)
└── third_party/
    ├── cws/                   # submódulo: framework HTTP
    └── c_gost_encryption/     # submódulo: criptografía GOST
```

## Submódulos

Este repo consume dos librerías como **git submodules**; no se editan aquí, se
versionan por commit y se actualizan con `git submodule`.

| Ruta | Repositorio | Propósito | Commit fijado |
|------|-------------|-----------|---------------|
| `third_party/cws` | [ulisesten/c_web_server](https://github.com/ulisesten/c_web_server) | Framework HTTP: router, middlewares, servidor estático, env, métricas | `1adddb4` (`devel`) |
| `third_party/c_gost_encryption` | [ulisesten/c_gost_encryption](https://github.com/ulisesten/c_gost_encryption) | Criptografía GOST: Streebog, 28147-89, 34.10-2012, `gost/kdf` (HMAC + PBKDF2) | `aedd242` (`main`) |

### Clonar

```bash
git clone --recurse-submodules <url-de-este-repo> cws_sodastream
# o, si ya está clonado sin submódulos:
git submodule update --init --recursive
```

### Actualizar un submódulo

```bash
# bajar el último commit de su rama y fijarlo
git submodule update --remote third_party/cws
git -C third_party/cws log --oneline -3        # revisar qué entra
git add third_party/cws
git commit -m "bump(cws): <commit>"
```

### Notas

- **Rama por submódulo**: `cws` sigue `devel`; `c_gost_encryption` sigue `main`.
  Al hacer `--remote`, cada `.gitmodules` define su rama.
- **SSH**: si el push del repo falla por HTTPS, configurar la clave y forzarla
  por repo: `git config core.sshCommand 'ssh -i ~/.ssh/id_ed25519 -o IdentitiesOnly=yes'`.
- **API consumida**:
  - `cws`: `cws_app_*`, `cws_router_*`, `cws_mw_*`, `cws_app_static_mount`,
    `cws_response_*`, `cws_env_*` (ver `third_party/cws/README.md`).
  - `c_gost_encryption`: `gost_hash`/`gost_hash_text`, `gost_cipher_cfb_*`,
    `gost_hmac`, `gost_pbkdf2` (ver `third_party/c_gost_encryption/README.md`).
- **CMake**: `CMakeLists.txt` hace `add_subdirectory(third_party/cws)` y
  `add_subdirectory(third_party/c_gost_encryption)` y enlaza `cws`, `gost::gost`
  y `ODBC::ODBC`.

## Build & run

```bash
./run.sh                 # instala dependencias, compila y ejecuta
./run.sh --skip-deps     # compila y ejecuta (sin tocar el sistema)
./run.sh --build-only    # solo compila
```

Manual:

```bash
cmake -S . -B build
cmake --build build -j
./build/cws_sodastream
```

El puerto sale de `.env` (`PORT`/`SERVER_PORT`, default `8080`). Pruebas
rápidas:

```bash
curl http://127.0.0.1:8080/healthz
curl http://127.0.0.1:8080/metrics
curl http://127.0.0.1:8080/api/v1/videos/popular
POST /api/v1/users/signin            # web: cookies + CSRF
POST /api/v1/auth/mobile/signin      # móvil: tokens en el body
```

Para consumir `cws` ya instalada en el sistema (en lugar del submódulo):

```cmake
find_package(cws REQUIRED CONFIG)
target_link_libraries(cws_sodastream PRIVATE cws::cws)
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
- **#2 Contraseñas con hash rápido y sin salt.** Ahora `core/password.{h,c}`
  usa **PBKDF2-HMAC-Streebog** (feature `gost/kdf`) con salt aleatorio de 16 B
  y pepper opcional del servidor (`PASSWORD_PEPPER`). Formato almacenado
  auto-descriptivo: `$gost-pbkdf2$512$<iter>$<salt_hex>$<dk_hex>`; iteraciones
  configurables con `PASSWORD_KDF_ITERATIONS` (default 100000 ≈ 270 ms).
  `password_verify()` sigue aceptando el formato legacy (Streebog-256) y avisa
  con `PASSWORD_OK_LEGACY` para rehashear. Requirió ampliar
  `cat_usuarios.usu_contrasena` y `@usu_contrasena` de `procUsersProc` a
  `NVARCHAR(255)` (ver `sql/migrations/`).

### ⬜ Pendientes

| # | Sev. | Tema | Dónde |
|---|------|------|-------|
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

