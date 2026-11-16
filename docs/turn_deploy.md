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

## 2. coturn (`/etc/turnserver.conf`)

```ini
listening-port=3478
tls-listening-port=5349
# 443 queda para nginx; si algún día se libera, se puede usar ahí.

use-auth-secret
static-auth-secret=<mismo TURN_SECRET del .env>
realm=sodastream.fun
external-ip=189.157.243.163

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
Con Let's Encrypt (nginx en 80):

```bash
certbot certonly --webroot -w /var/www/html -d turn.sodastream.fun
```

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

Prueba desde una red externa (no LAN) y con `turns` (TLS); en
`chrome://webrtc-internals` o `about:webrtc` verifica que se elige relay
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
