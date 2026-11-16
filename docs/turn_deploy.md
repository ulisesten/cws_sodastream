# Despliegue de STUN/TURN (coturn) para llamadas

Cierra el lado servidor de ICE/TURN. El API solo firma credenciales
temporales; coturn las valida y retransmite media.

## 1. DNS

Crea registros **A** hacia la IP pública del servidor:

```
stun.sodastream.fun  A  189.157.243.163
turn.sodastream.fun  A  189.157.243.163
```

No hace falta AAAA salvo que coturn escuche IPv6 y el firewall esté abierto.

## 1.1 Instalación y servicio

```bash
# Debian/Ubuntu
sudo apt update && sudo apt install coturn

# Fedora/RHEL
sudo dnf install coturn
```

Habilita el demonio:

```bash
# Debian/Ubuntu
sudo sed -i 's/^#\?TURNSERVER_ENABLED=.*/TURNSERVER_ENABLED=1/' /etc/default/coturn

# Fedora/RHEL
sudo sed -i 's/^TURNSERVER_ENABLED=.*/TURNSERVER_ENABLED=1/' /etc/sysconfig/turnserver

sudo systemctl enable --now coturn
sudo systemctl status coturn --no-pager
```

Si el paquete usa otro unit (`turnserver`), comprueba con
`systemctl list-unit-files | grep -i turn`.

### Permisos de certificados

coturn corre como `turnserver` y Let's Encrypt deja `live/` y `archive/` en
`root:root` con permisos restrictivos. Opciones:

```bash
# ACL (respeta symlinks de live/)
sudo setfacl -R -m u:turnserver:rX /etc/letsencrypt/live /etc/letsencrypt/archive
```

o copia los certificados a `/etc/coturn/certs/` con un hook de renovación y
permisos `turnserver:turnserver` 640, y apunta `cert`/`pkey` ahí.

En Fedora/RHEL con SELinux, si coturn no puede leer los archivos:

```bash
sudo semanage fcontext -a -t cert_t '/etc/coturn(/.*)?'
sudo restorecon -Rv /etc/coturn
```

## 2. coturn (`/etc/turnserver.conf`)

```ini
listening-port=3478
tls-listening-port=5349
# 443 queda para nginx; si algún día se libera, se puede usar ahí.

use-auth-secret
static-auth-secret=<mismo TURN_SECRET del .env>
realm=sodastream.fun
# Detrás de NAT: pública/privada. Si el host tiene la IP pública directa,
# basta con la pública.
external-ip=<IP-pública>/<IP-privada>

fingerprint
no-multicast-peers
no-tlsv1
no-tlsv1_1

denied-peer-ip=10.0.0.0-10.255.255.255
denied-peer-ip=172.16.0.0-172.31.255.255
denied-peer-ip=192.168.0.0-192.168.255.255
denied-peer-ip=127.0.0.0-127.255.255.255
denied-peer-ip=::1
denied-peer-ip=fc00::/7
denied-peer-ip=fe80::/10

cert=/etc/letsencrypt/live/turn.sodastream.fun/fullchain.pem
pkey=/etc/letsencrypt/live/turn.sodastream.fun/privkey.pem

min-port=49160
max-port=49200

# Opcional, recomendado en producción:
# total-quota=200
# user-quota=4
# max-bps=1000000
# log-file=/var/log/turnserver.log
# simple-log
```

- Si el servidor tiene IP privada detrás de NAT, usa
  `external-ip=189.157.243.163/<IP-privada>`.
- El usuario de coturn debe poder leer `cert`/`pkey`; si no, copia los
  certificados a `/etc/coturn/` con permisos `turnserver:turnserver` o usa
  ACLs y un hook de renovación.
- `static-auth-secret` debe ser **idéntico** a `TURN_SECRET` del API.
- `min-port`/`max-port` deben coincidir con el firewall.

### 2.1 IP pública dinámica

`external-ip` **no acepta nombres DNS**: coturn exige una IP numérica. Si tu
conexión tiene IP dinámica, un valor fijo deja de servir en cuanto cambia y
los clientes reciben un relay inalcanzable (`192.168.x.x`).

Solución: un script + timer que resuelve el DDNS, reescribe la línea y
reinicia coturn **solo cuando la IP cambia**.

```bash
# 1. Instala el script
sudo install -m 755 scripts/coturn-external-ip.sh /usr/local/sbin/coturn-external-ip.sh

# 2. Crea el servicio oneshot
sudo tee /etc/systemd/system/coturn-external-ip.service >/dev/null <<'EOF'
[Unit]
Description=Sincroniza external-ip de coturn con la IP pública
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=/usr/local/sbin/coturn-external-ip.sh
EOF

# 3. Crea el timer (cada minuto)
sudo tee /etc/systemd/system/coturn-external-ip.timer >/dev/null <<'EOF'
[Unit]
Description=Comprueba la IP pública de coturn cada minuto

[Timer]
OnBootSec=1min
OnUnitActiveSec=1min
AccuracySec=15s

[Install]
WantedBy=timers.target
EOF

# 4. Activa y aplica ya
sudo systemctl daemon-reload
sudo systemctl enable --now coturn-external-ip.timer
sudo /usr/local/sbin/coturn-external-ip.sh
systemctl list-timers coturn-external-ip.timer
```

> Si `enable` responde `Unit file ... does not exist`, faltó el paso 2 o 3:
> los archivos deben existir en `/etc/systemd/system/` antes de
> `daemon-reload`.

### Arranque tras apagón

Para que coturn arranque ya con la IP correcta, ejecuta el script **antes**
del servicio, sin reiniciarlo (`--no-restart`):

```bash
sudo mkdir -p /etc/systemd/system/coturn.service.d
sudo tee /etc/systemd/system/coturn.service.d/external-ip.conf >/dev/null <<'EOF'
[Service]
ExecStartPre=/usr/local/sbin/coturn-external-ip.sh --no-restart
EOF
sudo systemctl daemon-reload
sudo systemctl restart coturn
```

Si tu unit se llama `turnserver.service`, usa
`/etc/systemd/system/turnserver.service.d/external-ip.conf`.

Notas:

- La ruta por defecto depende de la distro: si existe
  `/etc/coturn/turnserver.conf` usa esa (Fedora/RHEL, o cuando el unit pasa
  `-c /etc/coturn/turnserver.conf`); si no, `/etc/turnserver.conf`. Comprueba
  cuál usa el servicio con `ps -ef | grep '[t]urnserver'` y fuérzala con
  `CONF=...` si hace falta.
- El script usa `turn.sodastream.fun` (tu DDNS) para obtener la pública y
  autodetecta la privada con `ip route get 1.1.1.1`; puedes forzarlas con
  `PUBLIC_HOST` y `PRIVATE_IP`.
- Funciona con `dig` o, si no está instalado, con `getent ahostsv4`.
- Reiniciar coturn **corta las asignaciones activas** (llamadas en curso). El
  timer solo reinicia cuando la IP cambió, no en cada ejecución.
- Si el router hace CGNAT (sin IP pública real), TURN no puede funcionar en
  ese host: usa un VPS con IP fija.
- Alternativa más simple y estable: mover coturn a un VPS con IP estática y
  dejar `external-ip=<IP-pública>` fija.

## 3. Firewall

Abre UDP/TCP 3478, TLS 5349 y el rango UDP de relay:

```bash
# firewalld
firewall-cmd --permanent --add-port=3478/udp
firewall-cmd --permanent --add-port=3478/tcp
firewall-cmd --permanent --add-port=5349/udp
firewall-cmd --permanent --add-port=5349/tcp
firewall-cmd --permanent --add-port=49160-49200/udp
firewall-cmd --reload

# ufw equivalente
ufw allow 3478/udp
ufw allow 3478/tcp
ufw allow 5349/udp
ufw allow 5349/tcp
ufw allow 49160:49200/udp
```

El rango de relay es **UDP**; no lo cierres. Si usas otro rango, cámbialo en
`min-port`/`max-port`.

## 4. Certificado TLS

`turn.sodastream.fun` necesita un certificado válido para `turns:`.
El error `"/var/www/html does not exist"` solo significa que pasaste un
webroot que no existe (en Fedora suele ser `/usr/share/nginx/html`).

**Opción recomendada (nginx en 80):**

```bash
sudo certbot certonly --nginx -d turn.sodastream.fun
```

Requiere el plugin de nginx (`python3-certbot-nginx` en Debian/Ubuntu,
`certbot-nginx` en Fedora). No usa `-w`.

**Opción webroot (si prefieres no tocar nginx):**

```bash
sudo mkdir -p /var/www/letsencrypt
```

Añade un server block temporal en nginx:

```nginx
server {
    listen 80;
    server_name turn.sodastream.fun;
    root /var/www/letsencrypt;

    location /.well-known/acme-challenge/ {
        root /var/www/letsencrypt;
    }
}
```

```bash
sudo nginx -t && sudo systemctl reload nginx
sudo certbot certonly --webroot -w /var/www/letsencrypt \
  -d turn.sodastream.fun
```

Comprueba antes que el DNS apunta al servidor y que el puerto 80 responde:

```bash
dig +short turn.sodastream.fun
curl -I http://turn.sodastream.fun/.well-known/acme-challenge/test
```

No uses `--standalone` si nginx ya ocupa el 80. El certificado queda en
`/etc/letsencrypt/live/turn.sodastream.fun/`.

Hook de renovación (`/etc/letsencrypt/renewal-hooks/deploy/coturn.sh`):

```sh
#!/bin/sh
# Si coturn lee directo de /etc/letsencrypt, basta el reload;
# si copias los archivos, cópialos aquí con permisos de turnserver.
systemctl reload coturn
```

`chmod +x` al hook. Si coturn no recarga el certificado, reinícialo.

## 5. `.env` del API

```dotenv
STUN_URLS=stun:stun.sodastream.fun:3478
TURN_URLS=turn:turn.sodastream.fun:3478?transport=udp,turns:turn.sodastream.fun:5349?transport=tcp
TURN_SECRET=<mismo static-auth-secret>
TURN_TTL_SECONDS=3600
```

- **No uses `turns:...:443`** si nginx ocupa 443.
- `TURN_SECRET` no se envía al cliente; solo se derivan usuario/credencial.
- Reinicia el API. Al arrancar debe imprimir:
  `calls: ICE STUN=configurado TURN=configurado TURN_SECRET=configurado`

## 6. Verificación

```bash
dig +short stun.sodastream.fun
dig +short turn.sodastream.fun
ss -lunpt | grep -E '3478|5349'
systemctl status coturn
journalctl -u coturn -n 50 --no-pager
```

Luego, con una llamada creada:

```bash
curl -H "Authorization: Bearer $TOKEN" \
  https://cws.sodastream.fun/api/v1/calls/<call_id>/ice
```

Debe devolver `ice_servers` con `stun:` en 3478 y `turns:` en **5349**, sin
`warning`.

Prueba desde una red externa (no LAN). Con `turnutils_uclient` puedes validar
las credenciales que devuelve `/calls/:id/ice`:

```bash
turnutils_uclient -v -u "<username>" -w "<credential>" -p 3478 turn.sodastream.fun
```

Para TLS en 5349 revisa `turnutils_uclient -h` (la opción de TLS varía según
la versión de coturn); también puedes verificarlo en
`chrome://webrtc-internals` o `about:webrtc` y confirmar que se elige relay
cuando la red lo requiere.

## 7. Problemas típicos

| Síntoma | Causa |
|---|---|
| `ice_servers: []` | faltan `STUN_URLS`/`TURN_URLS`/`TURN_SECRET` |
| `401` al asignar relay | `TURN_SECRET` no coincide con `static-auth-secret` |
| Timeout en TURN | firewall 3478/5349 o relay `49160-49200` cerrado |
| `turns` falla, `turn` funciona | certificado/permisos de `cert`/`pkey` o puerto 5349 |
| Relay no aparece | `min-port`/`max-port` no abiertos o `external-ip` incorrecto |
| Credencial expira en llamada larga | refresca `/calls/:id/ice` antes de `expires_in` |

## 8. Seguridad

- `TURN_SECRET` largo y aleatorio (32+ bytes); no en git/logs.
- TLS obligatorio para `turns`; deny peers privados/loopback.
- Cuotas (`total-quota`, `user-quota`, `max-bps`) para evitar abuso.
- Rota el secreto en coturn y API si se filtra; las credenciales viejas
  expiran por TTL.
- El contenido sigue E2EE GOST; TURN solo ve tráfico cifrado.
