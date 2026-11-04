# Cardputer ADV 原生 Rust tailnet 移植調查

目標是讓 Cardputer ADV 自己加入啟用 Tailnet Lock 的私人 tailnet，透過 Wi-Fi 向 Steam Deck 上的 `cardputer-claude` 服務送出文字與語音。這份文件記錄目前已核實的基線與尚未完成的移植工作；**尚無可供 Cardputer 安裝的 Rust 版本**。

## 選型與取捨

| 專案 | 原用途與技術棧 | 與本專案的差異 | 判斷 |
| --- | --- | --- | --- |
| [ESPHome Tailscale](https://github.com/Csontikka/esphome-tailscale) | ESPHome 感測器；ESP-IDF + C MicroLink，已有無 PSRAM 實驗模式 | [不支援 Tailnet Lock](https://github.com/Csontikka/esphome-tailscale#tailnet-lock)，目前主網路無法加入 | 只保留未鎖定測試網路的編譯實驗 |
| [0xdilo/tailscale-esp32](https://github.com/0xdilo/tailscale-esp32) | ESP-IDF + Rust 的單用途尾網設備，參考韌體為 Wake-on-LAN | 有節點金鑰輪替簽章邏輯，但沒有現成通用 TCP/IP 裝置介面；參考板需 8 MB PSRAM、16 MB flash | 作為 Rust 研究基礎，先驗證 Tailnet Lock 與無 PSRAM 可行性 |
| [esp32-tailbridge](https://github.com/pierrejay/esp32-tailbridge) | Arduino + WireGuard，需另設公開 WireGuard 代理伺服器 | 額外伺服器、連接埠轉發與網路命名空間；與原生加入 tailnet 的要求不同 | 不採用 |

## 已完成的最小驗證

- Steam Deck 的 Tailscale 正在運作，Tailnet Lock 啟用；目前約 20 個節點。
- 私人 `tailscale serve --bg 8790` 已設定。以本機測試 HTTP 服務驗證 `https://steamdeck.tailcb86e1.ts.net/` 回應 HTTP 200，憑證驗證通過，Funnel 未啟用。正式 `cardputer` plugin 必須由 `claude-cardputer` session 啟動。
- 2026-11-01 下載 `0xdilo/tailscale-esp32` 並執行 `cargo test --locked`：49 個測試通過，1 個需要即時 DERP 網路的測試被忽略。
- 已安裝 Espressif Rust Xtensa toolchain，將上游 Wake-on-LAN 參考韌體以本目錄的 `sdkconfig.defaults` 改成 8 MB flash、無 PSRAM，並成功為 `xtensa-esp32s3-espidf` 交叉編譯。產出的 ESP-IDF `sdkconfig` 已核對為 `CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y`、`CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y`，且未啟用 `CONFIG_SPIRAM`。這是使用佔位 Wi-Fi 與金鑰的**編譯驗證**，尚未在 Cardputer 開機或連上 tailnet。
- 2026-11-01 晚間 Cardputer ADV 已接上 `/dev/ttyACM1`（Espressif USB JTAG/serial，MAC `E8:F6:0A:98:36:B4`），使用者在 `uucp` 群組可直接存取。目前裝置上仍是 Arduino 版 `cardputer-claude` 韌體，尚未燒錄 Rust 映像。

### 交叉編譯設定

上游範例的 `sdkconfig.defaults` 不一定會被 `esp-idf-sys` 自動載入；必須明確指定 `ESP_IDF_SDKCONFIG_DEFAULTS`，並用全新的 `ESP_IDF_SDKCONFIG` 路徑避免舊的 2 MB 設定覆蓋它。`sdkconfig.defaults` 是本機移植設定，沒有 Wi-Fi 密碼或 auth key。參考韌體仍需依上游 README 建立本機 `.env`，填入測試用憑證；不要把 `.env` 提交到 Git。完整編譯後要再次檢查產出的 `sdkconfig`，不能只看此預設檔。

## Tailnet Lock peer 驗證（2026-11-01 進度）

工作分支：`~/Projects/tailscale-esp32-cardputer`，branch `cardputer-tailnet-lock`，基於上游 `dbd49fe`（MIT，上游最後提交 2026-07-23）。尚未 commit。設計與證據詳見該 repo 的 `docs/tailnet-lock.md`。

- 已移植 Go `tka` 的 peer 簽章驗證（direct／rotation／credential），CBOR 解碼與 Go 一樣嚴格。
- `NetworkMap` 只使用驗證通過的 peer；`PeersChangedPatch` 換 key 時若沒有新簽章，會被丟棄。
- 信任來源採**固定信任 key**（取自 `tailscale lock status`），未實作 AUM 同步。限制：tailnet 移除某把 lock key 後，Cardputer 需手動更新設定才會停止信任。
- 證據：60 個單元測試通過；22 組由 Go 官方 `tka` 產生的差分向量判定完全一致；用 Steam Deck 真實 netmap（1 self + 21 peers）驗證全部通過，只給部分 key 時拒絕數與 `tailscale lock status` 一致；`xtensa-esp32s3-espidf` 交叉編譯並連結成功。
- 尚未驗證：實機執行、ESP32-S3 每個 peer 的驗證耗時、鎖定網路上的新節點首次註冊流程。

## 移植前必須解決

1. **Tailnet Lock 首次註冊。** peer 驗證已完成（見上節），但新節點在鎖定網路註冊後須由 trusted key 簽署（`tailscale lock sign`）或使用預先簽署的 auth key，這個流程尚未實測。
2. **對外 HTTP。** 此 crate 明確表示不含通用 TCP/IP userspace stack；它的 `TcpConnector` 是連控制平面的底層 TCP，不等於可以讓 Arduino `HTTPClient` 連到 `100.x` peer。要接上既有 `/api/send`、`/api/poll`、`/api/voice`，需先建立可供應用使用的隧道 TCP 與 TLS 路徑，或提供安全的封包介面給 ESP-IDF 網路堆疊。
3. **記憶體與工具鏈。** Cardputer ADV 為 8 MB flash、無 PSRAM；上游 Wake-on-LAN 參考韌體設定 16 MB flash、8 MB PSRAM，且三個主要 Tailscale task 的 stack 設定合計超過 100 KB。必須先在 Cardputer 量測實際 heap，再整合顯示器、鍵盤、錄音與 HTTPS。
4. **保留 M5Launcher 與憑證安全。** 測試應產出可由 Launcher 安裝的 app binary；Wi-Fi 密碼、Tailscale auth key、Cardputer bearer token 不可寫入版本控制或固定燒進發佈映像。

## 下一個可驗證里程碑

先做僅含 Wi-Fi、Tailnet Lock 驗證與 peer 連通測試的 Cardputer ADV 原生 Rust 韌體，驗證它在這個約 20 節點的尾網可保持連線並量測 heap。通過後才加入對外 HTTPS、現有文字 API，最後接語音串流。這個順序能隔離 VPN、TCP 與錄音的故障來源。
