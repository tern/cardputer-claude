# Cardputer ADV 原生 tailnet 可行性實驗

**目前不可用於 Steam Deck 所在的 tailnet。** 該網路已啟用 Tailnet Lock；[ESPHome Tailscale 元件明確表示不支援](https://github.com/Csontikka/esphome-tailscale#tailnet-lock)，註冊後簽署節點或預先簽署 auth key 都不能解決。請勿為了這個實驗關閉 Tailnet Lock。

這裡保留一份最小連線韌體設定，供日後在**另一個未鎖定的測試 tailnet**驗證 Cardputer ADV（ESP32-S3FN8、無 PSRAM）的 Wi-Fi、記憶體與 VPN 連線。它不含 Claude Code 與語音介面，不取代 `firmware/` 的正式韌體。

## 已完成的建置驗證

- ESPHome 2026.10.1 的 `esphome config cardputer-adv.yaml` 通過。
- `esphome compile cardputer-adv.yaml` 通過，app binary 約 1.1 MB；編譯報告顯示靜態 RAM 使用 113,615 bytes。
- 尚未實機燒錄、量測執行時 heap、完成 Tailscale 註冊或測試跨網路傳輸。建置時只使用範本憑證。

## 若使用未鎖定的測試 tailnet

1. 安裝 ESPHome CLI 2026.3.1 或更新版本。
2. 複製 `secrets.yaml.example` 為 `secrets.yaml`，填入 2.4 GHz Wi-Fi 與測試 tailnet 的 auth key。不要將 `secrets.yaml` 加入 Git。
3. 執行 `esphome config cardputer-adv.yaml` 與 `esphome compile cardputer-adv.yaml`。
4. 將 `.esphome/build/cardputer-adv-tailnet-probe/build/firmware.ota.bin` 透過 M5Launcher 當作 app binary 安裝。不要把 `firmware.factory.bin` 安裝到 Launcher，也不要使用會覆蓋 Launcher 的全機燒錄流程。
5. 從序列埠確認裝置取得 `100.x` IP；再從同一測試 tailnet 的另一台裝置執行 `tailscale ping cardputer-adv-probe`。

`max_peers: 4` 可減少記憶體需求，但在大型 tailnet 中可能無法保留到要通訊的 peer。正式整合仍須解決 Tailnet Lock、Arduino 與 ESP-IDF 技術棧差異，以及無 PSRAM 時的執行時記憶體；上述編譯結果不代表現有語音韌體可直接走私人 tailnet。

## 目前更實際的私人連線路線

在外地使用已支援 Tailnet Lock 的 Tailscale 路由器或 Linux 裝置作為 Wi-Fi 閘道，讓 Cardputer 連它的 Wi-Fi，並將到 Steam Deck `100.x` 位址的流量轉送進 tailnet。這需要攜帶閘道設備；在沒有該設備的任意 Wi-Fi 上，Cardputer 仍無法直接進入此私人 tailnet。原生 Rust 客戶端的移植調查見 [rust-tailnet](../rust-tailnet/README.md)。
