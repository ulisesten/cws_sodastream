#!/usr/bin/env bash
#
# run.sh — instala dependencias, compila y ejecuta cws_sodastream.
#
# Uso:
#   ./run.sh                 instala dependencias, compila y ejecuta el server
#   ./run.sh --build-only    instala dependencias y compila (no ejecuta)
#   ./run.sh --skip-deps     compila y ejecuta sin tocar el sistema
#   ./run.sh --help          muestra esta ayuda
#
# Requiere una conexión a Internet la primera vez (instala paquetes y el
# driver ODBC de SQL Server). Las credenciales de BD se leen de .env.

set -euo pipefail

cd "$(dirname "$0")"

BUILD_ONLY=0
SKIP_DEPS=0

for arg in "$@"; do
    case "$arg" in
        --build-only) BUILD_ONLY=1 ;;
        --skip-deps)  SKIP_DEPS=1 ;;
        --help|-h)
            sed -n '2,12p' "$0"
            exit 0
            ;;
        *) echo "Opción desconocida: $arg" >&2; exit 1 ;;
    esac
done

# ---------------------------------------------------------------------------
# 1) Dependencias del sistema
# ---------------------------------------------------------------------------
install_deps() {
    local distro pkgman
    if [ -r /etc/os-release ]; then
        . /etc/os-release
        distro="${ID:-unknown}"
    else
        distro="unknown"
    fi

    case "$distro" in
        ubuntu|debian)
            pkgman="apt-get"
            "$pkgman" update -y
            "$pkgman" install -y --no-install-recommends \
                git cmake gcc make \
                unixodbc unixodbc-dev odbcinst

            # Microsoft ODBC Driver 17 for SQL Server
            curl -fsSL https://packages.microsoft.com/keys/microsoft.asc \
                >/etc/apt/trusted.gpg.d/microsoft.asc
            if ! grep -q packages.microsoft.com /etc/apt/sources.list /etc/apt/sources.list.d/* 2>/dev/null; then
                add-apt-repository \
                    "deb [arch=amd64] https://packages.microsoft.com/ubuntu/${VERSION_ID}/prod $(lsb_release -cs) main"
            fi
            "$pkgman" update -y
            ACCEPT_EULA=Y "$pkgman" install -y msodbcsql17
            ;;

        fedora|rhel|centos)
            pkgman="dnf"
            "$pkgman" install -y git cmake gcc make unixODBC unixODBC-devel curl

            # Microsoft ODBC Driver 17 for SQL Server
            if [ ! -f /etc/yum.repos.d/mssql-release.repo ]; then
                if [ "$distro" = "fedora" ]; then
                    curl -fSLo /etc/yum.repos.d/mssql-release.repo \
                        https://packages.microsoft.com/config/fedora/${VERSION_ID}/prod.repo
                else
                    curl -fSLo /etc/yum.repos.d/mssql-release.repo \
                        https://packages.microsoft.com/config/rhel/${VERSION_ID%.*}/prod.repo
                fi
            fi
            ACCEPT_EULA=Y "$pkgman" install -y msodbcsql17
            ;;

        *)
            echo "Distribución no soportada ($distro): instala manualmente" \
                 "cmake, gcc, unixODBC y 'ODBC Driver 17 for SQL Server'." >&2
            exit 1
            ;;
    esac
}

# ---------------------------------------------------------------------------
# 2) Submódulo de la lib cws
# ---------------------------------------------------------------------------
update_submodule() {
    # Si ya existe un checkout local de la lib (aunque esté "dirty" por
    # desarrollo), se respeta; si no, se baja el submódulo.
    if [ -n "$(ls -A third_party/cws 2>/dev/null)" ]; then
        echo "third_party/cws ya presente; se omite la actualización."
    else
        git submodule update --init --recursive
    fi
}

# ---------------------------------------------------------------------------
# 3) Compilación
# ---------------------------------------------------------------------------
build() {
    cmake -S . -B build
    cmake --build build -j
}

# ---------------------------------------------------------------------------
# 4) Arranque
# ---------------------------------------------------------------------------
run_server() {
    if [ ! -f .env ]; then
        echo "AVISO: no existe .env; copia un .env válido para el cliente BD." >&2
    fi
    echo "Arrancando cws_sodastream (Ctrl+C para detener)..."
    exec ./build/cws_sodastream
}

# ---------------------------------------------------------------------------

if [ "$SKIP_DEPS" -eq 0 ]; then
    install_deps
fi
update_submodule
build

if [ "$BUILD_ONLY" -eq 1 ]; then
    echo "Compilación lista en build/cws_sodastream"
    exit 0
fi
run_server
