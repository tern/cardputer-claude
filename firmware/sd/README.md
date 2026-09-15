# SD card files

Copy these two files to the root of a FAT32 microSD card in the Cardputer.

`/wifi.txt`
```
MyHotspotSSID
hotspot-password
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
