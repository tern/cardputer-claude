# cardputer-claude

用 M5Stack **Cardputer ADV** 在外頭對家裡／辦公室機器上的 **Claude Code** 下指令。

這個 repo 同時是一個 Claude Code plugin marketplace（`tern-mods`），
裡面有一個 channel plugin `cardputer`，以及 Cardputer ADV 的韌體。

```
Cardputer ADV ──HTTPS──▶ tailscale funnel ──▶ 127.0.0.1:8790 (server.mjs) ──MCP channel──▶ claude 互動 session
   (手機熱點)                                                                 ◀── reply 工具 / 權限詢問
```

跟官方 Telegram / Discord channel plugin 同一套機制：訊息直接推進「正在跑」的
互動 session（不是另開 `claude -p`），Claude 用 `reply` 工具把簡短回覆送回小螢幕，
權限詢問（例如要不要跑某個 Bash）也會推到 Cardputer，打 `y` / `n` 即可核准。

## 主機端（Omarchy、macOS 都一樣）

需求：Node ≥ 18、`claude` ≥ 2.1、Tailscale（或 cloudflared）。

```bash
claude plugin marketplace add tern-yu/cardputer-claude   # 或本機路徑 ~/Projects/cardputer-claude
claude plugin install cardputer@tern-mods
ln -s "$(pwd)/plugins/cardputer/bin/claude-cardputer" ~/.local/bin/   # 放進 PATH

claude-cardputer          # 啟動；第一次會問你要不要載入 development channel → 確認
```

第三方 channel 不在 Claude Code 的核准清單上，所以 launcher 用的是
`claude --dangerously-load-development-channels plugin:cardputer@tern-mods`。

啟動後 stderr 會印 `cardputer: http://127.0.0.1:8790`，token 在
`~/.claude/channels/cardputer/token`（自動產生）。

對外開放（一次設定，重開機後仍在）：

```bash
tailscale funnel --bg 8790      # → https://<主機>.<tailnet>.ts.net
```

先拿手機瀏覽器開 `https://<主機>.<tailnet>.ts.net/?token=<token>` 測試，
能收到回覆就代表主機端 OK。

在 session 裡打 `/cardputer` 可以看狀態與這些步驟。

## Cardputer ADV 端

1. microSD（FAT32）根目錄放兩個檔（範本在 `firmware/sd/`）：
   - `/wifi.txt`：第 1 行 SSID、第 2 行密碼（手機熱點）
   - `/claude.txt`：第 1 行 `https://<主機>.<tailnet>.ts.net`、第 2 行 token
2. 燒錄：
   ```bash
   cd firmware && pio run -t upload      # 第一次可能要：電源 OFF → 按住 G0 → 開機 進 download mode
   ```
3. 開機後畫面：`WiFi: ...` → `online` → `= 主機:目前目錄`。打字、Enter 送出。

| 輸入 | 作用 |
| --- | --- |
| 任意文字 + Enter | 送進 Claude Code session |
| `y` / `n` | 回答最新的權限詢問 |
| `:ping` | 顯示連到哪台主機、哪個目錄 |
| `:clear` | 清畫面 |
| `:wifi` | 重連 WiFi |

螢幕只有 240×135，plugin 已經在 MCP instructions 裡告訴 Claude：回覆要短、純文字、
可用繁中；細節留在終端機或寫進檔案再告訴你位置。

## 安全

- API 只綁 loopback，靠 funnel 對外；bearer token 是唯一的門，別貼進聊天。
- 拿到 token 的人可以核准工具權限，SD 卡請當密碼保管。
- 一個 session 佔一個 port；第二個 session 要用 `CARDPUTER_PORT=8791` 自己開一組。

## 目錄

```
.claude-plugin/marketplace.json   marketplace「tern-mods」
plugins/cardputer/                plugin：server.mjs（零依賴）、launcher、/cardputer skill
firmware/                         PlatformIO 專案（board: m5stack-stamps3，M5Cardputer 1.1.x）
```
