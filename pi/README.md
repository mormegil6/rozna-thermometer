# Pi side: public access without exposing the Arduino

The thermometer page is public at https://thermometer.bmroz.eu, but visitors never
reach the Arduino. A Raspberry Pi on the same LAN answers them from a cache and
forwards only a trickle of requests to the board.

```
visitor -> Cloudflare (HTTPS) -> cloudflared on the Pi -> nginx cache on the Pi (127.0.0.1:8081) -> Arduino (192.168.1.200)
```

The Arduino still serves the page, the scripts and `/list.json`. The Pi only caches
and relays. Nothing here changes the firmware.

## Why a cache is needed

The ENC28J60 is a 10 Mbit chip with one 1150 byte buffer, and every open browser tab
polls `/list.json` every 15 s. A handful of visitors, or one scanner, would keep it
busy or knock it over. With the cache in front, the board sees about one request per
10 s however many people watch.

What `nginx/thermometer.conf` and `nginx/thermo-proxy.conf` do:

- Listen on 127.0.0.1:8081 only. The Cloudflare tunnel is the only client.
- Proxy only the board's six URLs (`/`, `/list.json`, `/main.css`, `/main.js`,
  `/det.js`, `/favicon.svg`) and only GET. Everything else is refused by nginx itself
  (404, 400 or 403) and never reaches the board.
- Cache `/list.json` for 10 s, `/` for 60 s and the assets for 10 min (keyed by the
  `?v=` value, which must be 0 to 3 digits, so bumping `ASSET_VER` still works).
  Query strings on the other URLs are ignored, so they cannot bypass the cache.
- Collapse simultaneous cache misses into one request (`proxy_cache_lock`).
- Hold at most 2 connections open to the board (`max_conns=2`).
- Limit each visitor to 5 requests per second, burst 20, then answer 429. The visitor
  address comes from the `CF-Connecting-IP` header, trusted only from 127.0.0.1.
- Send the board a minimal request (`proxy_pass_request_headers off`). Cloudflare adds
  many headers, and the board's receive buffer is small.
- Keep the cache in RAM (`/dev/shm`) and write no access log.

Tested on the Pi: 800 requests from 800 different client addresses, 40 at a time,
caused exactly one request to the board. One address sending 100 requests as fast as
possible got 25 answers and 75 429s.

## Setup (Raspberry Pi OS or Debian, as root or with sudo)

### 1. nginx

```sh
sudo apt-get install nginx
sudo rm -f /etc/nginx/sites-enabled/default      # nothing should listen on port 80
sudo install -m 644 pi/nginx/thermometer.conf   /etc/nginx/conf.d/thermometer.conf
sudo install -m 644 pi/nginx/thermo-proxy.conf  /etc/nginx/snippets/thermo-proxy.conf
sudo nginx -t && sudo systemctl reload nginx
curl -i http://127.0.0.1:8081/list.json          # run twice: X-Cache goes MISS, then HIT
```

The board's address is in `thermometer.conf` (`192.168.1.200`) and in
`thermo-proxy.conf` (the `Host` header). Change both if the board's IP changes.

### 2. cloudflared

Debian has no `cloudflared` package, so add Cloudflare's own apt repository:

```sh
sudo mkdir -p --mode=0755 /usr/share/keyrings
curl -fsSL https://pkg.cloudflare.com/cloudflare-public-v2.gpg | sudo tee /usr/share/keyrings/cloudflare-public-v2.gpg >/dev/null
echo "deb [signed-by=/usr/share/keyrings/cloudflare-public-v2.gpg] https://pkg.cloudflare.com/cloudflared any main" | sudo tee /etc/apt/sources.list.d/cloudflared.list
sudo apt-get update && sudo apt-get install cloudflared
```

The domain's DNS must already be on Cloudflare. Then:

```sh
cloudflared tunnel login                 # prints a link; open it, pick the domain, authorize
cloudflared tunnel create thermometer    # prints the tunnel id, writes ~/.cloudflared/<id>.json
cloudflared tunnel route dns thermometer thermometer.bmroz.eu    # creates the CNAME
sudo mkdir -p /etc/cloudflared
sudo install -m 600 -o root -g root ~/.cloudflared/<id>.json /etc/cloudflared/<id>.json
# write /etc/cloudflared/config.yml from pi/cloudflared/config.yml.example, then:
sudo cloudflared service install
systemctl is-active cloudflared          # active
curl -i https://thermometer.bmroz.eu/list.json
```

`cert.pem` from the login lets the Pi create tunnels and DNS records for the whole
domain, and the running tunnel does not need it. Delete `~/.cloudflared/cert.pem`
once the tunnel works and log in again only when you add another hostname.

## Do not

- Forward a router port straight to the Arduino. It speaks plain HTTP with no
  authentication and a tiny TCP stack.
- Point anything public at the Arduino's address instead of the Pi.

## Behaviour when the board is down

After the 10 s cache expires visitors get an error from nginx. An already open page
keeps showing its last reading and retries every 15 s; it has no "stale" marker, so
the footer uptime standing still is the only hint.
