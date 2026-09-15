# SD card files

Copy these two files to the root of a FAT32 microSD card in the Cardputer.

`/wifi.txt` — ssid / password line pairs, up to 8 networks, first match in range wins.
Optional: the `:wifi` picker on the device writes this file for you.
```
MyHotspotSSID
hotspot-password
HomeWifi
home-password
```

`/claude.txt`
```
https://steamdeck.tail1234.ts.net
<token from ~/.claude/channels/cardputer/token>
```

Optional third line in `/claude.txt`: `insecure` skips TLS certificate
verification (needed for cloudflared quick tunnels; not needed for Tailscale
Funnel, whose Let's Encrypt chain is built in). A plain `http://` URL works
too for LAN testing when the server is started with `CARDPUTER_HOST=0.0.0.0`.
