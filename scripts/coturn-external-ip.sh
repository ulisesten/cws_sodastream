#!/bin/sh
# Mantiene `external-ip` de coturn sincronizado con la IP pública actual.
#
# coturn no acepta nombres DNS en -X/--external-ip: exige una IP numérica
# (o `pública/privada` si está detrás de NAT). Con IP dinámica hay que
# reescribir la línea y reiniciar coturn cuando cambie.
#
# Uso:
#   sudo ./coturn-external-ip.sh
#
# Variables:
#   CONF=/etc/turnserver.conf      configuración de coturn
#   PUBLIC_HOST=turn.sodastream.fun  nombre DDNS que sigue la IP pública
#   RESOLVER=1.1.1.1               DNS para resolver el nombre
#   PRIVATE_IP=192.168.1.78        IP local del host (se autodetecta si falta)
set -eu

CONF=${CONF:-/etc/turnserver.conf}
PUBLIC_HOST=${PUBLIC_HOST:-turn.sodastream.fun}
RESOLVER=${RESOLVER:-1.1.1.1}
PRIVATE_IP=${PRIVATE_IP:-}

# Evita dos ejecuciones a la vez (systemd timer + arranque).
if command -v flock >/dev/null 2>&1; then
    exec 9>/run/coturn-external-ip.lock
    flock -n 9 || exit 0
fi

if [ -z "$PRIVATE_IP" ]; then
    PRIVATE_IP=$(ip -4 route get 1.1.1.1 2>/dev/null |
        awk '{for (i = 1; i <= NF; i++) if ($i == "src") { print $(i + 1); exit }}')
fi

PUBLIC_IP=$(dig +short A "$PUBLIC_HOST" "@$RESOLVER" 2>/dev/null |
    grep -E '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' | head -n 1)

if [ -z "$PUBLIC_IP" ]; then
    echo "ERROR: $PUBLIC_HOST no tiene registro A" >&2
    exit 1
fi
if [ -z "$PRIVATE_IP" ]; then
    echo "ERROR: no se pudo detectar la IP privada" >&2
    exit 1
fi

WANT="external-ip=$PUBLIC_IP/$PRIVATE_IP"

# Nada que hacer si ya está exactamente así.
if [ "$(grep -c '^external-ip=' "$CONF" 2>/dev/null || true)" -eq 1 ] &&
   grep -qxF "$WANT" "$CONF"; then
    exit 0
fi

cp -a "$CONF" "$CONF.bak"
sed -i '/^external-ip=/d' "$CONF"
printf '%s\n' "$WANT" >> "$CONF"

if command -v systemctl >/dev/null 2>&1; then
    systemctl restart coturn
else
    echo "Reinicia coturn manualmente para aplicar $WANT" >&2
fi

echo "coturn: $WANT"
