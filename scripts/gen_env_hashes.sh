#!/usr/bin/env bash
#
# gen_env_hashes.sh — genera la tabla hash de cfg_getenv.
#
# Corre la MISMA función hash (hash_string en core/hash_table.c, FNV-1a 32)
# vía el binario hashgen, para obtener los case <hash> que se pegan en el
# switch de configuration.c.
#
# Uso:
#   ./scripts/gen_env_hashes.sh                 # claves del .env (o default)
#   ./scripts/gen_env_hashes.sh PORT DB_USER    # claves explícitas
#
# La salida se pega dentro de cfg_getenv() en core/configuration.c.

set -euo pipefail
cd "$(dirname "$0")/.."

if [ $# -gt 0 ]; then
    KEYS=("$@")
else
    # Claves del .env; si no existe, el set default del proyecto.
    if [ -f .env ]; then
        mapfile -t KEYS < <(grep -oE '^[A-Za-z_][A-Za-z0-9_]*' .env | sort -u)
    else
        KEYS=(PORT SERVER_PORT DB_DRIVER DB_SERVER DB_PORT DB_USER
              DB_PASSWORD DB_DATABASE DB_ENCRYPT DB_TRUST_CERTIFICATE
              SECRET_KEY X_VECTOR ACCESS_TOKEN_EXPIRATION_MINUTES
              REFRESH_TOKEN_EXPIRATION_DAYS NODE_ENV HLS_DIR)
    fi
fi
if [ ${#KEYS[@]} -eq 0 ]; then
    echo "sin claves: pasa KEY1 KEY2 ... o agrega entradas a .env" >&2
    exit 1
fi

# Compilar el generador (misma función que el runtime).
if [ ! -x build/hashgen ]; then
    echo "compilando hashgen..." >&2
    cmake -S . -B build >/dev/null
    cmake --build build --target hashgen >/dev/null
fi

./build/hashgen "${KEYS[@]}"
