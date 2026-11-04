# microSD 設定檔

將以下設定檔放在 Cardputer 的 FAT32 microSD 根目錄。

`/claude-wifi.txt` — SSID 與密碼每兩行一組，最多 8 組；也可以在裝置上用 `:wifi` 建立。使用獨立檔名，避免改寫 Bruce 的 `/wifi.txt`。
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

`/claude.txt` 可加第三行 `insecure` 以略過 TLS 憑證驗證。Tailscale Funnel 不需要此設定。區網測試可使用 `http://`，主機端需設 `CARDPUTER_HOST=0.0.0.0`。
