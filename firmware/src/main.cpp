// Cardputer ADV client for the Claude Code "cardputer" channel plugin.
//
// Type a line, press Enter: it is POSTed to /api/send and lands in the
// interactive Claude Code session as a channel message. A background task
// long-polls /api/poll and prints replies / permission prompts on screen.
// Answer a permission prompt by typing just `y` or `n`.
//
// Local commands: `:ping` (who am I talking to), `:clear`, `:wifi` (reconnect).

#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <deque>
#include "certs.h"

// SD SPI pins are the same on Cardputer and Cardputer-ADV (M5Unified pin table).
#define SD_SPI_SCK_PIN 40
#define SD_SPI_MISO_PIN 39
#define SD_SPI_MOSI_PIN 14
#define SD_SPI_CS_PIN 12

#define INPUT_H 16
#define UI_FONT &fonts::efontTW_12  // CJK-capable so Claude may answer in Chinese

struct Config {
    String ssid, pass, baseUrl, token;
    bool insecure = false;
} cfg;

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
    d.print("> " + shown + "_");
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

bool loadConfig() {
    String w[2];
    if (!readLines("/wifi.txt", w, 2)) {
        showLine("Missing /wifi.txt (ssid, password)", TFT_RED);
        return false;
    }
    cfg.ssid = w[0];
    cfg.pass = w[1];

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

bool connectWifi() {
    showLine("WiFi: " + cfg.ssid + " ...", TFT_DARKGREY);
    WiFi.mode(WIFI_STA);
    WiFi.begin(cfg.ssid.c_str(), cfg.pass.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > 20000) {
            showLine("WiFi: timed out (:wifi to retry)", TFT_RED);
            return false;
        }
        delay(250);
    }
    showLine("WiFi: " + WiFi.localIP().toString(), TFT_DARKGREY);
    return true;
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
            wasOnline = true;
            handleEvents(out);
        } else {
            online = false;
            if (wasOnline || code == 401) enqueue("! poll " + String(code) + " " + clip(out, 60), TFT_RED);
            wasOnline = false;
            vTaskDelay(pdMS_TO_TICKS(code == 401 ? 15000 : 3000));
        }
    }
}

// ---- UI task -------------------------------------------------------------
void ping() {
    String out;
    int code = apiUi.request("GET", "/api/ping", "", out, 10000);
    if (code != 200) {
        showLine("! ping " + String(code) + " " + clip(out, 60), TFT_RED);
        return;
    }
    JsonDocument doc;
    if (!deserializeJson(doc, out)) {
        showLine(String("= ") + (const char *)(doc["host"] | "?") + ":" + (const char *)(doc["cwd"] | "?"), TFT_CYAN);
        if (doc["pending"].size() > 0) permPending = true;
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
    if (line == ":wifi") {
        WiFi.disconnect();
        connectWifi();
        return;
    }

    showLine("> " + line, TFT_CYAN);
    String out;
    int code = apiUi.request("POST", "/api/send", line, out, 15000);
    if (code != 200) showLine("! send " + String(code) + " " + clip(out, 60), TFT_RED);
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
        }

        // Drain what the poll task collected.
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
    bool ready = false;
    if (!SD.begin(SD_SPI_CS_PIN, SPI, 25000000)) {
        showLine("SD card not found", TFT_RED);
    } else if (loadConfig() && connectWifi()) {
        ready = true;
    }

    if (ready) {
        apiUi.begin();
        xTaskCreatePinnedToCore(pollTask, "poll", 16384, nullptr, 1, nullptr, 0);
        ping();
    }
    drawInput();
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
