// Cardputer ADV client for the Claude Code "cardputer" channel plugin.
//
// Type a line, press Enter: it is POSTed to /api/send and lands in the
// interactive Claude Code session as a channel message. A background task
// long-polls /api/poll and prints replies / permission prompts on screen.
// Answer a permission prompt by typing just `y` or `n`.
//
// Local commands: `:ping` (who am I talking to), `:clear`, `:wifi` (on-device
// network picker: scan, choose, type the password; saved to /wifi.txt).

#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <deque>
#include <vector>
#include <algorithm>
#include "certs.h"

// SD SPI pins are the same on Cardputer and Cardputer-ADV (M5Unified pin table).
#define SD_SPI_SCK_PIN 40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_SPI_CS_PIN 12

#define INPUT_H 16
#define UI_FONT &fonts::efontTW_12  // CJK-capable so Claude may answer in Chinese

struct WifiProfile {
    String ssid, pass;
};

#define MAX_WIFI_PROFILES 8

struct Config {
    std::vector<WifiProfile> wifi;  // /wifi.txt: ssid/password line pairs, most recent first
    String baseUrl, token;
    bool insecure = false;
} cfg;

// UI modes: normal chat, or the on-device WiFi picker (:wifi).
enum class Mode { Chat, WifiList, WifiPass };
Mode mode = Mode::Chat;

// ---- state shared between the UI task and the poll task ----------------
SemaphoreHandle_t stateMutex;
std::deque<std::pair<String, uint16_t>> incoming;  // (text, colour) waiting to be printed
volatile bool permPending = false;
volatile long lastSeq = 0;
volatile bool online = false;

void enqueue(const String &text, uint16_t color) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    incoming.emplace_back(text, color);
    xSemaphoreGive(stateMutex);
}

// ---- display -------------------------------------------------------------
M5Canvas canvas(&M5Cardputer.Display);
String input;

void showLine(const String &s, uint16_t color = TFT_WHITE) {
    Serial.println(s);
    canvas.setTextColor(color, TFT_BLACK);
    canvas.println(s);
    canvas.pushSprite(0, 0);
}

void drawInput() {
    auto &d = M5Cardputer.Display;
    int y = d.height() - INPUT_H;
    d.fillRect(0, y, d.width(), INPUT_H, 0x2104 /* dark grey */);
    String shown = input;
    // Keyboard input is ASCII, so trimming by bytes is safe here.
    while (shown.length() > 0 && d.textWidth("> " + shown + "_") > d.width() - 4) shown.remove(0, 1);
    d.setTextColor(permPending ? TFT_YELLOW : TFT_WHITE, 0x2104);
    d.setCursor(2, y + 2);
    const char *prompt = mode == Mode::WifiPass ? "pw> " : mode == Mode::WifiList ? "#> " : "> ";
    d.print(prompt + shown + "_");
}

// ---- config from SD ------------------------------------------------------
bool readLines(const char *path, String lines[], int maxLines) {
    File f = SD.open(path);
    if (!f) return false;
    int n = 0;
    while (f.available() && n < maxLines) {
        String l = f.readStringUntil('\n');
        l.trim();
        lines[n++] = l;
    }
    f.close();
    return n > 0;
}

void loadWifiProfiles() {
    cfg.wifi.clear();
    String w[MAX_WIFI_PROFILES * 2];
    if (!readLines("/wifi.txt", w, MAX_WIFI_PROFILES * 2)) return;
    for (int i = 0; i + 1 < MAX_WIFI_PROFILES * 2 && !w[i].isEmpty(); i += 2) cfg.wifi.push_back({w[i], w[i + 1]});
}

bool saveWifiProfiles() {
    SD.remove("/wifi.txt");
    File f = SD.open("/wifi.txt", FILE_WRITE);
    if (!f) return false;
    for (auto &p : cfg.wifi) {
        f.println(p.ssid);
        f.println(p.pass);
    }
    f.close();
    return true;
}

// Insert/move a network to the front so it is tried first next boot.
void rememberWifi(const String &ssid, const String &pass) {
    for (size_t i = 0; i < cfg.wifi.size(); ++i)
        if (cfg.wifi[i].ssid == ssid) cfg.wifi.erase(cfg.wifi.begin() + i--);
    cfg.wifi.insert(cfg.wifi.begin(), {ssid, pass});
    while (cfg.wifi.size() > MAX_WIFI_PROFILES) cfg.wifi.pop_back();
    if (!saveWifiProfiles()) showLine("! could not write /wifi.txt", TFT_RED);
}

bool loadConfig() {
    loadWifiProfiles();
    if (cfg.wifi.empty()) showLine("No /wifi.txt yet - use :wifi", TFT_YELLOW);

    String c[3];
    if (!readLines("/claude.txt", c, 3)) {
        showLine("Missing /claude.txt (url, token)", TFT_RED);
        return false;
    }
    cfg.baseUrl = c[0];
    while (cfg.baseUrl.endsWith("/")) cfg.baseUrl.remove(cfg.baseUrl.length() - 1);
    cfg.token = c[1];
    cfg.insecure = c[2] == "insecure";
    if (!cfg.baseUrl.startsWith("http") || cfg.token.isEmpty()) {
        showLine("/claude.txt: line1=https://... line2=token", TFT_RED);
        return false;
    }
    return true;
}

bool joinWifi(const String &ssid, const String &pass, int timeoutMs = 20000) {
    showLine("WiFi: " + ssid + " ...", TFT_DARKGREY);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, true);
    delay(100);
    WiFi.begin(ssid.c_str(), pass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > (unsigned long)timeoutMs) {
            showLine("WiFi: failed (" + ssid + ")", TFT_RED);
            return false;
        }
        delay(250);
    }
    showLine("WiFi: " + WiFi.localIP().toString(), TFT_DARKGREY);
    return true;
}

// Scan results, kept for the picker. Sorted by signal strength.
struct Network {
    String ssid;
    int rssi;
    bool open;
};
std::vector<Network> networks;
int selected = 0;
String pendingSsid;

void scanNetworks() {
    showLine("scanning ...", TFT_DARKGREY);
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks();
    networks.clear();
    for (int i = 0; i < n; ++i) {
        String s = WiFi.SSID(i);
        if (s.isEmpty()) continue;
        bool dup = false;
        for (auto &k : networks) if (k.ssid == s) { dup = true; break; }
        if (dup) continue;
        networks.push_back({s, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
    }
    std::sort(networks.begin(), networks.end(), [](const Network &a, const Network &b) { return a.rssi > b.rssi; });
    WiFi.scanDelete();
}

// Boot: join the first saved network that is actually in range.
bool connectKnownWifi() {
    if (cfg.wifi.empty()) return false;
    scanNetworks();
    for (auto &p : cfg.wifi)
        for (auto &k : networks)
            if (k.ssid == p.ssid && joinWifi(p.ssid, p.pass)) return true;
    showLine("no saved network in range", TFT_YELLOW);
    return false;
}

bool isKnown(const String &ssid) {
    for (auto &p : cfg.wifi) if (p.ssid == ssid) return true;
    return false;
}

void drawWifiList() {
    canvas.fillScreen(TFT_BLACK);
    canvas.setCursor(0, 0);
    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    canvas.println("WiFi  (fn+; up  fn+. down  Enter  ` back)");
    const int rows = 6;
    int top = selected < rows ? 0 : selected - rows + 1;
    for (int i = top; i < (int)networks.size() && i < top + rows; ++i) {
        auto &k = networks[i];
        bool cur = i == selected;
        canvas.setTextColor(cur ? TFT_BLACK : (isKnown(k.ssid) ? TFT_GREENYELLOW : TFT_WHITE), cur ? TFT_WHITE : TFT_BLACK);
        String line = String(cur ? ">" : " ") + (k.open ? " " : "*") + String(k.rssi) + " " + k.ssid;
        if (isKnown(k.ssid)) line += " (saved)";
        while (line.length() > 0 && canvas.textWidth(line) > canvas.width()) line.remove(line.length() - 1);
        canvas.println(line);
    }
    if (networks.empty()) {
        canvas.setTextColor(TFT_RED, TFT_BLACK);
        canvas.println("nothing found - Enter to rescan");
    }
    canvas.pushSprite(0, 0);
}

void enterChatMode() {
    mode = Mode::Chat;
    input = "";
    canvas.fillScreen(TFT_BLACK);
    canvas.setCursor(0, 0);
    canvas.setTextColor(TFT_WHITE, TFT_BLACK);
    canvas.pushSprite(0, 0);
    drawInput();
}

void enterWifiList() {
    mode = Mode::WifiList;
    input = "";
    selected = 0;
    scanNetworks();
    // Preselect the strongest saved network so Enter just works.
    for (size_t i = 0; i < networks.size(); ++i)
        if (isKnown(networks[i].ssid)) { selected = i; break; }
    drawWifiList();
    drawInput();
}

void finishWifiJoin(const String &ssid, const String &pass) {
    enterChatMode();
    if (joinWifi(ssid, pass)) rememberWifi(ssid, pass);
}

// Enter pressed in the picker: open network joins now, otherwise ask for a
// password (prefilled with the saved one, so Enter reuses it).
void pickNetwork() {
    if (networks.empty()) { enterWifiList(); return; }
    auto &k = networks[selected];
    pendingSsid = k.ssid;
    if (k.open) { finishWifiJoin(k.ssid, ""); return; }
    mode = Mode::WifiPass;
    input = "";
    for (auto &p : cfg.wifi) if (p.ssid == k.ssid) input = p.pass;
    canvas.setTextColor(TFT_CYAN, TFT_BLACK);
    canvas.println("password for " + k.ssid + ":");
    canvas.setTextColor(TFT_DARKGREY, TFT_BLACK);
    canvas.println("(Enter = join, ` = back)");
    canvas.pushSprite(0, 0);
    drawInput();
}

// ---- HTTP client (one per task: TLS state is not shareable) -------------
class Api {
public:
    void begin() {
        if (cfg.insecure) tls.setInsecure();
        else tls.setCACert(ISRG_ROOT_X1);
        http.setReuse(true);
    }

    // Returns the HTTP status (or a negative HTTPClient error). Body in `out`.
    int request(const char *method, const String &path, const String &body, String &out, int timeoutMs) {
        WiFiClient &client = cfg.baseUrl.startsWith("https") ? static_cast<WiFiClient &>(tls) : plain;
        http.setTimeout(timeoutMs);
        if (!http.begin(client, cfg.baseUrl + path)) return -100;
        http.addHeader("Authorization", "Bearer " + cfg.token);
        int code;
        if (strcmp(method, "POST") == 0) {
            http.addHeader("Content-Type", body.startsWith("{") ? "application/json" : "text/plain; charset=utf-8");
            code = http.POST(body);
        } else {
            code = http.GET();
        }
        out = code > 0 ? http.getString() : http.errorToString(code);
        http.end();
        return code;
    }

private:
    WiFiClientSecure tls;
    WiFiClient plain;
    HTTPClient http;
};

Api apiUi;    // send / perm / ping, from the UI task
Api apiPoll;  // long-poll, from the poll task

// ---- poll task -----------------------------------------------------------
String clip(const String &s, unsigned n) { return s.length() > n ? s.substring(0, n) + "..." : s; }

void handleEvents(const String &json) {
    JsonDocument doc;
    if (deserializeJson(doc, json)) {
        enqueue("! bad json from server", TFT_RED);
        return;
    }
    for (JsonObject e : doc["events"].as<JsonArray>()) {
        const char *type = e["type"] | "";
        if (!strcmp(type, "reply")) {
            enqueue(String("< ") + (const char *)(e["text"] | ""), TFT_GREENYELLOW);
        } else if (!strcmp(type, "edit")) {
            enqueue(String("(edit) ") + (const char *)(e["text"] | ""), TFT_GREENYELLOW);
        } else if (!strcmp(type, "perm")) {
            permPending = true;
            enqueue(String("? PERM ") + (const char *)(e["tool_name"] | "") + ": " +
                        clip((const char *)(e["input_preview"] | ""), 160) + "  [y/n]",
                    TFT_YELLOW);
        } else if (!strcmp(type, "perm_done")) {
            permPending = false;
            enqueue(String("perm: ") + (const char *)(e["behavior"] | ""), TFT_YELLOW);
        }
        // "sent" is the echo of our own message; the UI already printed it.
    }
    long s = doc["seq"] | 0L;
    if (s > lastSeq) lastSeq = s;
}

void pollTask(void *) {
    apiPoll.begin();
    bool wasOnline = false;
    int failures = 0;
    for (;;) {
        if (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        String out;
        int code = apiPoll.request("GET", "/api/poll?after=" + String(lastSeq) + "&wait=20", "", out, 30000);
        if (code == 200) {
            online = true;
            if (!wasOnline) enqueue("online", TFT_DARKGREY);
            failures = 0;
            wasOnline = true;
            handleEvents(out);
        } else {
            online = false;
            // Report the first failure after being online, a bad token, or a
            // streak of three so a dead tunnel is not silent.
            failures = wasOnline ? 1 : failures + 1;
            if (wasOnline || code == 401 || failures == 3) enqueue("! poll " + String(code) + " " + clip(out, 60), TFT_RED);
            wasOnline = false;
            vTaskDelay(pdMS_TO_TICKS(code == 401 ? 15000 : 3000));
        }
    }
}

// ---- UI task -------------------------------------------------------------
void ping() {
    String out;
    int code = -1;
    // The first request right after WiFi comes up tends to fail (DNS / TLS
    // not ready yet), so give it a few tries before reporting.
    for (int attempt = 0; attempt < 3 && code < 0; ++attempt) {
        if (attempt) delay(1000);
        code = apiUi.request("GET", "/api/ping", "", out, 10000);
    }
    if (code != 200) {
        showLine("! ping " + String(code) + " " + clip(out, 60), TFT_RED);
        return;
    }
    JsonDocument doc;
    if (!deserializeJson(doc, out)) {
        showLine(String("= ") + (const char *)(doc["host"] | "?") + ":" + (const char *)(doc["cwd"] | "?"), TFT_CYAN);
        if (doc["pending"].size() > 0) permPending = true;
        // Fresh boot: replay only the last few events instead of the server's whole buffer.
        long s = doc["seq"] | 0L;
        if (lastSeq == 0 && s > 3) lastSeq = s - 3;
    }
}

void submit(String line) {
    line.trim();
    if (line.isEmpty()) return;

    if (permPending && (line == "y" || line == "n")) {
        String out;
        String body = String("{\"request_id\":\"latest\",\"behavior\":\"") + (line == "y" ? "allow" : "deny") + "\"}";
        int code = apiUi.request("POST", "/api/perm", body, out, 10000);
        if (code != 200) showLine("! perm " + String(code) + " " + clip(out, 60), TFT_RED);
        else permPending = false;
        return;
    }
    if (line == ":ping") return ping();
    if (line == ":clear") {
        canvas.fillScreen(TFT_BLACK);
        canvas.setCursor(0, 0);
        canvas.pushSprite(0, 0);
        return;
    }
    if (line == ":wifi") return enterWifiList();

    showLine("> " + line, TFT_CYAN);
    String out;
    int code = apiUi.request("POST", "/api/send", line, out, 15000);
    if (code != 200) showLine("! send " + String(code) + " " + clip(out, 60), TFT_RED);
}

bool hasChar(const std::vector<char> &word, char c) {
    for (char w : word) if (w == c) return true;
    return false;
}

// Keys in the picker: fn+; / fn+. (the arrow legends) move, Enter picks,
// ` (fn+` is ESC on the legend) backs out. Bare ; and . work too so the
// picker is usable one-handed.
void handleWifiListKeys(const Keyboard_Class::KeysState &st) {
    if (hasChar(st.word, '`')) return enterChatMode();
    if (hasChar(st.word, ';') && selected > 0) selected--;
    if (hasChar(st.word, '.') && selected + 1 < (int)networks.size()) selected++;
    if (st.enter) return pickNetwork();
    drawWifiList();
    drawInput();
}

void handleWifiPassKeys(const Keyboard_Class::KeysState &st) {
    if (hasChar(st.word, '`')) return enterWifiList();
    for (auto c : st.word) input += c;
    if (st.del && !input.isEmpty()) input.remove(input.length() - 1);
    if (st.enter) {
        String pass = input;
        finishWifiJoin(pendingSsid, pass);
        return;
    }
    drawInput();
}

void handleChatKeys(const Keyboard_Class::KeysState &st) {
    for (auto c : st.word) input += c;
    if (st.del && !input.isEmpty()) input.remove(input.length() - 1);
    if (st.enter) {
        String line = input;
        input = "";
        drawInput();
        submit(line);
    }
    drawInput();
}

void uiLoop() {
    unsigned long lastKeyMillis = 0;
    const unsigned long debounceMs = 120;
    for (;;) {
        M5Cardputer.update();

        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            unsigned long now = millis();
            if (now - lastKeyMillis >= debounceMs) {
                lastKeyMillis = now;
                auto st = M5Cardputer.Keyboard.keysState();
                switch (mode) {
                    case Mode::WifiList: handleWifiListKeys(st); break;
                    case Mode::WifiPass: handleWifiPassKeys(st); break;
                    default: handleChatKeys(st); break;
                }
            }
        }

        // Drain what the poll task collected (only onto the chat screen;
        // the picker owns the canvas while it is open).
        if (mode == Mode::Chat) {
            xSemaphoreTake(stateMutex, portMAX_DELAY);
            while (!incoming.empty()) {
                auto item = incoming.front();
                incoming.pop_front();
                xSemaphoreGive(stateMutex);
                showLine(item.first, item.second);
                drawInput();
                xSemaphoreTake(stateMutex, portMAX_DELAY);
            }
            xSemaphoreGive(stateMutex);
        }

        delay(10);
    }
}

// Everything runs on tasks with generous stacks: the default Arduino loop
// task (8KB) is too small for a TLS handshake, and the two HTTP clients live
// on separate tasks so a 20s long-poll never blocks typing.
void uiTask(void *) {
    auto &d = M5Cardputer.Display;
    d.setRotation(1);
    d.setBrightness(80);
    d.setFont(UI_FONT);
    d.fillScreen(TFT_BLACK);

    canvas.createSprite(d.width(), d.height() - INPUT_H);
    canvas.setFont(UI_FONT);
    canvas.setTextWrap(true);
    canvas.setTextScroll(true);
    canvas.fillScreen(TFT_BLACK);

    showLine("claude channel", TFT_DARKGREY);
    drawInput();

    SPI.begin(SD_SPI_SCK_PIN, SD_SPI_MISO_PIN, SD_SPI_MOSI_PIN, SD_SPI_CS_PIN);
    bool haveConfig = false;
    if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
        showLine("SD card not found", TFT_RED);
    } else {
        haveConfig = loadConfig();
    }

    if (haveConfig) {
        // The poll task idles until WiFi is up, so start it regardless.
        apiUi.begin();
        xTaskCreatePinnedToCore(pollTask, "poll", 16384, nullptr, 1, nullptr, 0);
    }

    if (connectKnownWifi()) {
        if (haveConfig) ping();
        drawInput();
    } else {
        enterWifiList();
    }
    uiLoop();
}

void setup() {
    Serial.begin(115200);
    auto mcfg = M5.config();
    M5Cardputer.begin(mcfg, true);
    stateMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(uiTask, "ui", 16384, nullptr, 1, nullptr, 1);
}

void loop() {
    vTaskDelete(nullptr);
}
