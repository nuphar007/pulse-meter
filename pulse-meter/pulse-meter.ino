#include <WiFi.h>
#include <PubSubClient.h>
#include <Ticker.h>
#include <Preferences.h>
#include <LiquidCrystal_PCF8574.h>
#include <WebServer.h>
#include <Wire.h>
#include <HTTPUpdate.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>   // OTA over HTTPS
#include <ESPmDNS.h>            // #2 mDNS hostname
#include <esp_task_wdt.h>       // #4 hardware watchdog
#include <esp_ota_ops.h>        // #10 mark-app-valid / rollback support

// Some ESP32 board variants don't define LED_BUILTIN; default to GPIO2 (common onboard LED).
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

#ifndef FIRMWARE_VERSION
// Local-build fallback shown on the LCD when you compile in the Arduino IDE.
// Bump this to match the release tag you're flashing. CI auto-overrides it with
// the git tag, so OTA builds always show the exact released version.
#define FIRMWARE_VERSION "v1.55"
#endif

// Revision history is tracked via git tags / GitHub Releases:
//   https://github.com/nuphar007/pulse-meter/releases

// GitHub repo used for the auto-update channel (#5) and "update to latest" button.
#define OTA_REPO "nuphar007/pulse-meter"
// Always-latest release asset URL (GitHub redirects this to the newest release).
#define OTA_LATEST_URL "https://github.com/" OTA_REPO "/releases/latest/download/firmware.bin"

// #7: when "OTA secure" is on, OTA is restricted to the official GitHub releases
// (blocks an attacker who can publish to the command feed from pushing arbitrary firmware).
#define OTA_OFFICIAL_PREFIX "https://github.com/" OTA_REPO "/releases/"

// Watchdog: reboots the device if loop() ever hangs longer than this. Generous so
// normal blocking work (boot connect, OTA download) never trips it (#4).
#define WDT_TIMEOUT_S 60

Preferences preferences; // Create a Preferences object for NVS
Ticker pulseTimer;       // Ticker for handling pulses

// WiFi credentials - now as variables that can be modified
String ssid = "Yu.me Wifi";
String password = "";   // #8: no hardcoded default; provision via NVS / AP mode

// Adafruit IO credentials
String ioUsername = "Yume86";   // Default; loaded from NVS, settable via the web form
// NOTE: the AIO key is never hardcoded. It is entered via the web form and stored
// in NVS (key "aioKey"), then read at runtime in connectToMQTT().
#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  1883 // MQTT port

// Unique Device ID - now loaded from NVS
String deviceID = "spare"; // Default device ID

// --- Operational settings (NVS-backed) ---
String configPassword = "";       // #3: web UI password (HTTP Basic). Empty = no auth.
bool   autoUpdateEnabled = false; // #5: auto-update from GitHub latest. Off by default.
bool   otaSecure = false;         // #7: validate TLS cert chain for OTA. Off = insecure (proven).

// Create MQTT client with PubSubClient
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Define GPIO Pins
const int inputDollars = 25;   // D02 for Dollars input
const int inputWins = 14;      // D06 for Wins input
const int resetButton = 17;    // D10 for Reset input
const int gamesOutput = 16;    // D11 for Games output

// Counters for each input
volatile unsigned long dollarsCount = 0;
volatile unsigned long winsCount = 0;
volatile unsigned long gamesOutputCount = 0;   // Count for Games output pulses

// LCD Setup
LiquidCrystal_PCF8574 lcd(0x27);   // Set the LCD address to 0x27

// Queue for output pulses based on input counters
volatile bool dollarsStateHigh = false;
volatile bool winsStateHigh = false;
volatile unsigned long outputQueueCount = 0;

// Debounce timing - updated to use microseconds
unsigned long debounceTime = 60;  // Will be loaded from NVS
unsigned long lastDebounceTimeDollars = 0;
unsigned long lastDebounceTimeWins = 0;

// Long press reset timing
const unsigned long resetHoldTime = 3000;  // 3 seconds to reset
unsigned long resetButtonPressTime = 0;
volatile bool resetPressed = false;

// NVS Preferences keys
const char* nvsNamespace = "pulseCounter";

// Track when a change is first detected for NVS save
static unsigned long changeDetectedTime = 0;

// #6: RAM shadow of last-saved counters (avoids reading NVS every loop iteration)
unsigned long savedDollarsCount = 0;
unsigned long savedWinsCount = 0;
unsigned long savedGamesCount = 0;

// Serial print interval variables
unsigned long previousPrintTime = 0;   // Tracks the last time Serial printed
const unsigned long printInterval = 1000; // Set interval for Serial print (1 second)

// WiFi reconnection interval
const unsigned long wifiReconnectInterval = 30000; // re-begin if down this long
unsigned long lastWifiReconnectAttempt = 0;
bool wifiWasConnected = false;

// Track last sent values for conditional publishing
unsigned long lastSentDollarsCount = 0;
unsigned long lastSentWinsCount = 0;

// Flags for ISR handling
volatile bool dollarsISRFlag = false;
volatile bool winsISRFlag = false;

// OTA / boot bookkeeping
bool otaJustUpdated = false;       // set in setup from NVS flag
bool bootAnnounced = false;        // booted/OTA-success message sent once MQTT is up
unsigned long lastAutoUpdateCheck = 0;
const unsigned long autoUpdateInterval = 6UL * 60 * 60 * 1000; // every 6 hours

// Web server
WebServer server(80);

// Topic paths
String dollarFeedPath;
String winFeedPath;
String statusFeedPath;
String commandFeedPath;

// ---------- LCD helpers (#9: richer status, no flicker) ----------
void lcdPrintRow(int row, const String &s) {
  String t = s;
  if (t.length() > 20) t = t.substring(0, 20);
  while (t.length() < 20) t += ' ';
  lcd.setCursor(0, row);
  lcd.print(t);
}

void updateLCDStatus(const char* message) {
  // Bottom row, used for transient status messages.
  lcdPrintRow(3, String(message));
}

String uptimeString() {
  unsigned long s = millis() / 1000;
  if (s >= 86400) return String(s / 86400) + "d" + String((s % 86400) / 3600) + "h";
  if (s >= 3600)  return String(s / 3600) + "h" + String((s % 3600) / 60) + "m";
  if (s >= 60)    return String(s / 60) + "m";
  return String(s) + "s";
}

// ---------- OTA (#1 web, #5 auto, #7 integrity) ----------
void configureOTAClient(WiFiClientSecure &client) {
  // Transport: GitHub HTTPS. Cert pinning is unreliable across GitHub's redirect host,
  // so we keep the proven insecure transport and gate integrity on the source URL instead.
  client.setInsecure();
}

void performOTA(const String &url, const char *source) {
  Serial.printf("OTA from %s: %s\n", source, url.c_str());

  // #7: official-source allowlist (only enforced when otaSecure is on).
  if (otaSecure && !url.startsWith(OTA_OFFICIAL_PREFIX)) {
    Serial.println("OTA blocked: URL not from official releases.");
    if (mqtt.connected()) mqtt.publish(commandFeedPath.c_str(), "RESP: OTA blocked: untrusted URL (otasecure=on)");
    updateLCDStatus("OTA blocked");
    return;
  }
  updateLCDStatus("OTA: downloading");
  if (mqtt.connected()) {
    String m = String("RESP: OTA starting (") + source + "), downloading...";
    mqtt.publish(commandFeedPath.c_str(), m.c_str());
    mqtt.loop();
  }
  delay(100);
  esp_task_wdt_reset(); // feed before the long, blocking download

  WiFiClientSecure secureClient;
  configureOTAClient(secureClient);

  httpUpdate.rebootOnUpdate(false); // reboot manually so we can confirm success
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  t_httpUpdate_return ret = httpUpdate.update(secureClient, url);
  esp_task_wdt_reset();

  switch (ret) {
    case HTTP_UPDATE_FAILED: {
      String err = "RESP: OTA FAILED (" + String(httpUpdate.getLastError()) + "): "
                   + httpUpdate.getLastErrorString();
      Serial.println(err);
      if (mqtt.connected()) mqtt.publish(commandFeedPath.c_str(), err.c_str());
      updateLCDStatus("OTA Failed");
      break;
    }
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("OTA: No update available.");
      if (mqtt.connected()) mqtt.publish(commandFeedPath.c_str(), "RESP: OTA: no update available");
      updateLCDStatus("OTA No Update");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("OTA: downloaded. Rebooting to apply...");
      preferences.putBool("otaPending", true);
      updateLCDStatus("OTA OK: reboot");
      if (mqtt.connected()) {
        mqtt.publish(commandFeedPath.c_str(), "RESP: OTA downloaded OK, rebooting...");
        mqtt.loop();
      }
      delay(200);
      ESP.restart();
      break;
  }
}

// #5: check GitHub for a newer release and self-update if found.
void checkAutoUpdate(bool manual) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!manual && !autoUpdateEnabled) return;

  Serial.println("Auto-update: checking latest release...");
  updateLCDStatus("Checking update");
  esp_task_wdt_reset();

  WiFiClientSecure client;
  configureOTAClient(client);
  HTTPClient http;
  String api = String("https://api.github.com/repos/") + OTA_REPO + "/releases/latest";
  if (!http.begin(client, api)) return;
  http.addHeader("User-Agent", "pulse-meter");
  http.setTimeout(8000);
  int code = http.GET();

  if (code == 200) {
    String body = http.getString();
    int tagIdx = body.indexOf("\"tag_name\"");
    if (tagIdx >= 0) {
      int colon = body.indexOf(':', tagIdx);
      int s = body.indexOf('"', colon + 1);
      int e = (s >= 0) ? body.indexOf('"', s + 1) : -1;
      if (s >= 0 && e > s) {
        String latest = body.substring(s + 1, e);
        Serial.println("Latest: " + latest + ", current: " FIRMWARE_VERSION);
        if (latest.length() > 0 && latest != String(FIRMWARE_VERSION)) {
          String url = String("https://github.com/") + OTA_REPO + "/releases/download/" + latest + "/firmware.bin";
          http.end();
          if (mqtt.connected())
            mqtt.publish(commandFeedPath.c_str(),
                         (String("RESP: Auto-update ") + FIRMWARE_VERSION + " -> " + latest).c_str());
          performOTA(url, "auto");
          return;
        } else if (manual && mqtt.connected()) {
          mqtt.publish(commandFeedPath.c_str(), "RESP: Already on latest (" FIRMWARE_VERSION ")");
        }
      }
    }
  } else {
    Serial.printf("Auto-update check failed: HTTP %d\n", code);
    if (manual && mqtt.connected())
      mqtt.publish(commandFeedPath.c_str(), ("RESP: Update check failed: HTTP " + String(code)).c_str());
  }
  http.end();
}

// ---------- WiFi / mDNS ----------
void onWiFiConnected() {
  updateLCDStatus("WiFi OK");
  Serial.println("\nWiFi connected: " + WiFi.localIP().toString());
  // #2: mDNS — reach the device at http://<deviceID>.local
  MDNS.end();
  if (MDNS.begin(deviceID.c_str())) {
    MDNS.addService("http", "tcp", 80);
    Serial.println("mDNS: http://" + deviceID + ".local");
  }
}

void startAPMode() {
  const char* apSSID = "ESP32_Config";
  const char* apPassword = "12345678";
  WiFi.softAP(apSSID, apPassword);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(ip);
  updateLCDStatus("AP Mode");
  lcd.setCursor(0, 2);
  lcd.print("IP: ");
  lcd.print(ip.toString());
}

void connectToWiFi() {
  updateLCDStatus("WiFi Connect...");
  Serial.print("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 30000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    onWiFiConnected();
    wifiWasConnected = true;
  } else {
    Serial.println("\nFailed to connect to WiFi. Switching to AP mode.");
    startAPMode();
  }
}

// ---------- Web UI (#1 dashboard + OTA, #3 masking + auth) ----------
bool requireAuth() {
  if (configPassword.length() == 0) return true;       // auth disabled
  if (!server.authenticate("admin", configPassword.c_str())) {
    server.requestAuthentication();
    return false;
  }
  return true;
}

void handleRoot() {
  if (!requireAuth()) return;

  String storedSSID = preferences.getString("ssid", ssid);
  String storedDeviceID = preferences.getString("deviceID", deviceID);
  String storedIOUser = preferences.getString("ioUser", ioUsername);
  bool hasKey = preferences.getString("aioKey", "").length() > 0;
  bool hasWifiPw = preferences.getString("password", "").length() > 0;

  String rssi = (WiFi.status() == WL_CONNECTED) ? String(WiFi.RSSI()) + " dBm" : "—";
  String ip   = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "AP mode";

  String h;
  h += "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  h += "<title>" + deviceID + " — pulse-meter</title><style>";
  h += "body{font-family:system-ui,Arial,sans-serif;max-width:560px;margin:0 auto;padding:16px;background:#0f172a;color:#e2e8f0}";
  h += "h1{font-size:20px}h2{font-size:15px;margin:18px 0 6px;color:#93c5fd}";
  h += ".card{background:#1e293b;border:1px solid #334155;border-radius:10px;padding:12px;margin:10px 0}";
  h += ".grid{display:grid;grid-template-columns:1fr 1fr;gap:8px}";
  h += ".stat b{display:block;font-size:20px}.stat span{font-size:11px;color:#94a3b8}";
  h += "label{display:block;font-size:12px;color:#94a3b8;margin-top:8px}";
  h += "input[type=text],input[type=password],input[type=number]{width:100%;box-sizing:border-box;padding:8px;margin-top:2px;border-radius:6px;border:1px solid #475569;background:#0f172a;color:#e2e8f0}";
  h += "button,input[type=submit]{margin-top:10px;padding:9px 14px;border:0;border-radius:6px;background:#2563eb;color:#fff;font-weight:600}";
  h += "small{color:#64748b}</style></head><body>";

  h += "<h1>" + deviceID + " <small>" FIRMWARE_VERSION "</small></h1>";
  h += "<div class='card grid'>";
  h += "<div class='stat'><b>$" + String(dollarsCount) + "</b><span>Dollars</span></div>";
  h += "<div class='stat'><b>" + String(winsCount) + "</b><span>Wins</span></div>";
  h += "<div class='stat'><b>" + String(mqtt.connected() ? "online" : "offline") + "</b><span>MQTT</span></div>";
  h += "<div class='stat'><b>" + rssi + "</b><span>WiFi</span></div>";
  h += "<div class='stat'><b>" + uptimeString() + "</b><span>Uptime</span></div>";
  h += "<div class='stat'><b>" + ip + "</b><span>IP</span></div>";
  h += "</div>";

  // Firmware update (#1)
  h += "<h2>Firmware update (OTA)</h2><div class='card'>";
  h += "<form action='/update' method='post'>";
  h += "<label>Firmware URL</label><input type='text' name='url' value='" OTA_LATEST_URL "'>";
  h += "<small>Default pulls the latest GitHub release. Device reboots if successful.</small><br>";
  h += "<input type='submit' value='Update firmware'></form></div>";

  // Settings (#3 masking: secrets not pre-filled)
  h += "<h2>Settings</h2><div class='card'><form action='/saveSettings' method='post'>";
  h += "<label>Device ID</label><input type='text' name='deviceID' value='" + storedDeviceID + "'>";
  h += "<label>WiFi SSID</label><input type='text' name='ssid' value='" + storedSSID + "'>";
  h += "<label>WiFi Password " + String(hasWifiPw ? "(set — blank keeps current)" : "") + "</label>";
  h += "<input type='password' name='password' placeholder='" + String(hasWifiPw ? "••••••••" : "WiFi password") + "'>";
  h += "<label>Adafruit IO Username</label><input type='text' name='ioUser' value='" + storedIOUser + "'>";
  h += "<label>Adafruit IO Key " + String(hasKey ? "(set — blank keeps current)" : "") + "</label>";
  h += "<input type='password' name='aioKey' placeholder='" + String(hasKey ? "••••••••" : "aio_...") + "'>";
  h += "<label>Config page password (blank = no login)</label>";
  h += "<input type='password' name='cfgPass' placeholder='" + String(configPassword.length() ? "•••••• (blank keeps current)" : "set a password") + "'>";
  h += "<label><input type='checkbox' name='autoUpd' style='width:auto'" + String(autoUpdateEnabled ? " checked" : "") + "> Auto-update from GitHub latest</label>";
  h += "<label><input type='checkbox' name='otaSecure' style='width:auto'" + String(otaSecure ? " checked" : "") + "> Only allow OTA from official GitHub releases</label>";
  h += "<input type='submit' value='Save & reboot'></form></div>";

  // Counters + debounce
  h += "<h2>Counters</h2><div class='card'><form action='/setCounters' method='post'>";
  h += "<label>Dollars</label><input type='number' name='dollars' value='" + String(dollarsCount) + "'>";
  h += "<label>Wins</label><input type='number' name='wins' value='" + String(winsCount) + "'>";
  h += "<input type='submit' value='Update counters'></form></div>";

  h += "<h2>Debounce</h2><div class='card'><form action='/setDebounce' method='post'>";
  h += "<label>Debounce (ms)</label><input type='number' name='debounce' value='" + String(debounceTime) + "'>";
  h += "<input type='submit' value='Update debounce'></form></div>";

  h += "</body></html>";
  server.send(200, "text/html", h);
}

void handleSaveSettings() {
  if (!requireAuth()) return;
  if (!(server.hasArg("deviceID") && server.hasArg("ssid"))) {
    server.send(400, "text/plain", "Bad Request");
    return;
  }
  String newDeviceID = server.arg("deviceID");
  newDeviceID.toLowerCase();
  preferences.putString("deviceID", newDeviceID);
  preferences.putString("ssid", server.arg("ssid"));

  // #3: only overwrite secrets when a new value is provided (blank = keep current)
  if (server.hasArg("password") && server.arg("password").length() > 0)
    preferences.putString("password", server.arg("password"));
  if (server.hasArg("ioUser"))
    preferences.putString("ioUser", server.arg("ioUser"));
  if (server.hasArg("aioKey") && server.arg("aioKey").length() > 0)
    preferences.putString("aioKey", server.arg("aioKey"));
  if (server.hasArg("cfgPass") && server.arg("cfgPass").length() > 0)
    preferences.putString("cfgPass", server.arg("cfgPass"));

  // Checkboxes: present only when ticked
  preferences.putBool("autoUpd", server.hasArg("autoUpd"));
  preferences.putBool("otaSecure", server.hasArg("otaSecure"));

  Serial.println("Settings saved via web.");
  server.send(200, "text/html", "<html><body><h2>Saved. Rebooting…</h2></body></html>");
  delay(1000);
  ESP.restart();
}

void handleSetCounters() {
  if (!requireAuth()) return;
  if (server.hasArg("dollars") && server.hasArg("wins")) {
    dollarsCount = server.arg("dollars").toInt();
    winsCount = server.arg("wins").toInt();
    preferences.putULong("dollarsCount", dollarsCount);
    preferences.putULong("winsCount", winsCount);
    savedDollarsCount = dollarsCount;
    savedWinsCount = winsCount;
    Serial.printf("Counters updated via web: $=%lu, Wins=%lu\n", dollarsCount, winsCount);
  }
  server.send(200, "text/html", "<p>Counters updated. <a href='/'>Back</a></p>");
}

void handleSetDebounce() {
  if (!requireAuth()) return;
  if (server.hasArg("debounce")) {
    debounceTime = server.arg("debounce").toInt();
    preferences.putULong("debounceTime", debounceTime);
    updateLCDStatus("Debounce OK");
  }
  server.send(200, "text/html", "<p>Debounce updated. <a href='/'>Back</a></p>");
}

void handleUpdate() {
  if (!requireAuth()) return;
  String url = server.hasArg("url") ? server.arg("url") : String(OTA_LATEST_URL);
  server.send(200, "text/html",
              "<html><body><h2>OTA started.</h2><p>The device will reboot if successful. "
              "<a href='/'>Back</a></p></body></html>");
  delay(200);
  performOTA(url, "web");
}

void startWebServer() {   // #6: all routes registered once, in one place
  server.on("/", HTTP_GET, handleRoot);
  server.on("/saveSettings", HTTP_POST, handleSaveSettings);
  server.on("/setCounters", HTTP_POST, handleSetCounters);
  server.on("/setDebounce", HTTP_POST, handleSetDebounce);
  server.on("/update", HTTP_POST, handleUpdate);
  server.begin();
  Serial.println("HTTP server started");
}

// ---------- MQTT ----------
void sendPulseData(bool forceSend = false) {
  if (!mqtt.connected()) {
    Serial.println("MQTT not connected. Cannot send data.");
    return;
  }
  bool dataChanged = false;
  if (dollarsCount != lastSentDollarsCount || forceSend) {
    if (!mqtt.publish(dollarFeedPath.c_str(), String(dollarsCount).c_str())) {
      Serial.println("Failed to send dollar pulses");
    } else { lastSentDollarsCount = dollarsCount; dataChanged = true; }
  }
  if (winsCount != lastSentWinsCount || forceSend) {
    if (!mqtt.publish(winFeedPath.c_str(), String(winsCount).c_str())) {
      Serial.println("Failed to send win pulses");
    } else { lastSentWinsCount = winsCount; dataChanged = true; }
  }
  if (dataChanged || forceSend) {
    Serial.println("Data sent to Adafruit IO: $=" + String(dollarsCount) + ", Wins=" + String(winsCount));
  }
}

void performCounterReset(const char* reason = "Manual") {
  noInterrupts();
  dollarsCount = 0;
  winsCount = 0;
  gamesOutputCount = 0;
  outputQueueCount = 0;
  dollarsStateHigh = digitalRead(inputDollars);
  winsStateHigh = digitalRead(inputWins);
  lastDebounceTimeDollars = micros();
  lastDebounceTimeWins = micros();
  interrupts();

  preferences.putULong("dollarsCount", dollarsCount);
  preferences.putULong("winsCount", winsCount);
  preferences.putULong("gamesOutputCount", gamesOutputCount);
  savedDollarsCount = 0; savedWinsCount = 0; savedGamesCount = 0;

  updateLCDStatus("Reset Done");
  Serial.printf("Counters reset (%s).\n", reason);
  sendPulseData(true);
}

void sendStatus() {
  if (mqtt.connected()) {
    if (mqtt.publish(statusFeedPath.c_str(), "online")) Serial.println("Status sent: online");
  }
}

// Single, non-blocking MQTT connect attempt (#4). Returns true on success.
bool mqttConnectOnce() {
  if (WiFi.status() != WL_CONNECTED) return false;
  String aioKey = preferences.getString("aioKey", "");
  if (aioKey == "") { updateLCDStatus("Missing AIO Key"); return false; }

  char clientID[50];
  snprintf(clientID, sizeof(clientID), "ESP32_%s_%u", deviceID.c_str(), random(0xffff));
  updateLCDStatus("MQTT Connect...");
  if (mqtt.connect(clientID, ioUsername.c_str(), aioKey.c_str())) {
    updateLCDStatus("MQTT OK");
    Serial.println("MQTT connected.");
    mqtt.subscribe(commandFeedPath.c_str());
    mqtt.publish(statusFeedPath.c_str(), "online");
    return true;
  }
  updateLCDStatus("MQTT Retry");
  return false;
}

// Called right after MQTT connects (setup or loop) — announce boot/OTA once.
void onMqttConnected() {
  sendPulseData(true);
  if (!bootAnnounced) {
    if (otaJustUpdated)
      mqtt.publish(commandFeedPath.c_str(), "RESP: OTA SUCCESS: now running " FIRMWARE_VERSION);
    else
      mqtt.publish(commandFeedPath.c_str(), "RESP: Booted " FIRMWARE_VERSION);
    bootAnnounced = true;
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  Serial.print("MQTT command received [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  if (message.startsWith("RESP:")) return;  // ignore self-responses

  if (message.startsWith("update:")) {
    performOTA(message.substring(7), "MQTT");
    return;
  }
  else if (message == "update" || message == "update:latest") {
    performOTA(OTA_LATEST_URL, "MQTT-latest");
    return;
  }
  else if (message == "checkupdate") {
    checkAutoUpdate(true);
  }
  else if (message.startsWith("set:autoupdate=")) {
    autoUpdateEnabled = message.endsWith("on");
    preferences.putBool("autoUpd", autoUpdateEnabled);
    mqtt.publish(commandFeedPath.c_str(), (String("RESP: autoupdate=") + (autoUpdateEnabled ? "on" : "off")).c_str());
  }
  else if (message.startsWith("set:otasecure=")) {
    otaSecure = message.endsWith("on");
    preferences.putBool("otaSecure", otaSecure);
    mqtt.publish(commandFeedPath.c_str(), (String("RESP: otasecure=") + (otaSecure ? "on" : "off")).c_str());
  }
  else if (message.startsWith("set:dollars=")) {
    dollarsCount = message.substring(String("set:dollars=").length()).toInt();
    preferences.putULong("dollarsCount", dollarsCount);
    savedDollarsCount = dollarsCount;
    updateLCDStatus("Set $ via MQTT");
    sendPulseData(true);
    mqtt.publish(commandFeedPath.c_str(), ("RESP: Confirmed: dollars=" + String(dollarsCount)).c_str());
  }
  else if (message.startsWith("set:wins=")) {
    winsCount = message.substring(String("set:wins=").length()).toInt();
    preferences.putULong("winsCount", winsCount);
    savedWinsCount = winsCount;
    updateLCDStatus("Set Wins MQTT");
    sendPulseData(true);
    mqtt.publish(commandFeedPath.c_str(), ("RESP: Confirmed: wins=" + String(winsCount)).c_str());
  }
  else if (message.startsWith("set:debounce=")) {
    debounceTime = message.substring(String("set:debounce=").length()).toInt();
    preferences.putULong("debounceTime", debounceTime);
    updateLCDStatus("Set Debounce");
    mqtt.publish(commandFeedPath.c_str(), ("RESP: Confirmed: debounce=" + String(debounceTime) + " ms").c_str());
  }
  else if (message == "on")  { digitalWrite(LED_BUILTIN, HIGH); }
  else if (message == "off") { digitalWrite(LED_BUILTIN, LOW); }
  else if (message == "reset") { performCounterReset("MQTT"); }
  else if (message == "status") {
    String r = "RESP: Status OK\n";
    r += "dollars=" + String(dollarsCount) + "\n";
    r += "wins=" + String(winsCount) + "\n";
    r += "debounce=" + String(debounceTime) + " ms\n";
    r += "rssi=" + String(WiFi.RSSI()) + " dBm\n";
    r += "uptime=" + uptimeString() + "\n";
    r += "ip=" + WiFi.localIP().toString() + "\n";
    r += "autoupdate=" + String(autoUpdateEnabled ? "on" : "off") + "\n";
    r += "Firmware=" FIRMWARE_VERSION;
    mqtt.publish(commandFeedPath.c_str(), r.c_str());
    updateLCDStatus("Status OK");
  }
  else if (message.startsWith("get:")) {
    String p = message.substring(4), r;
    if (p == "dollars") r = "RESP: Dollars=" + String(dollarsCount);
    else if (p == "wins") r = "RESP: Wins=" + String(winsCount);
    else if (p == "debounce") r = "RESP: Debounce=" + String(debounceTime);
    else r = "RESP: Unknown parameter: " + p;
    mqtt.publish(commandFeedPath.c_str(), r.c_str());
  }
  else if (message == "?") {
    String m = "RESP: Available Commands:\n";
    m += "update / update:<url> - OTA (latest or URL)\n";
    m += "checkupdate - check GitHub latest now\n";
    m += "set:autoupdate=on|off - auto-update toggle\n";
    m += "set:otasecure=on|off - official-only OTA\n";
    m += "set:dollars=<val> - Set dollar counter\n";
    m += "set:wins=<val> - Set win counter\n";
    m += "set:debounce=<ms> - Set debounce in ms\n";
    m += "reset - Reset all counters to 0\n";
    m += "on / off - Built-in LED\n";
    m += "status - Status, IP, RSSI, uptime\n";
    m += "get:dollars|wins|debounce - Read a value\n";
    m += "version - Firmware version";
    mqtt.publish(commandFeedPath.c_str(), m.c_str());
    updateLCDStatus("MQTT Help Sent");
  }
  else if (message == "version") {
    mqtt.publish(commandFeedPath.c_str(), "RESP: Firmware=" FIRMWARE_VERSION);
    updateLCDStatus(FIRMWARE_VERSION);
  }
  else {
    Serial.println("Unknown command.");
  }
}

// ====================================================================
//  Pulse counting — ISR / debounce / output. DO NOT MODIFY (validated).
// ====================================================================
void debounceHandler(int pin, volatile unsigned long &lastDebounceTime, volatile unsigned long &count, volatile bool &stateHigh, volatile bool &isrFlag) {
  unsigned long currentTime = micros(); // Use microseconds for more precise debouncing
  unsigned long timeDifference;
  if (currentTime >= lastDebounceTime) {
    timeDifference = currentTime - lastDebounceTime;
  } else {
    timeDifference = (ULONG_MAX - lastDebounceTime) + currentTime + 1;
  }
  if (timeDifference > debounceTime * 1000) { // Convert debounceTime to microseconds
    bool currentState = digitalRead(pin);
    if (!stateHigh && currentState == HIGH) {
      stateHigh = true;
      isrFlag = true; // Set flag instead of directly incrementing counter
    } else if (stateHigh && currentState == LOW) {
      stateHigh = false;
      isrFlag = true; // Set flag instead of directly incrementing counter
    }
    lastDebounceTime = currentTime;
  }
}

void IRAM_ATTR dollarsISR() {
  debounceHandler(inputDollars, lastDebounceTimeDollars, dollarsCount, dollarsStateHigh, dollarsISRFlag);
}

void IRAM_ATTR winsISR() {
  debounceHandler(inputWins, lastDebounceTimeWins, winsCount, winsStateHigh, winsISRFlag);
}

void pulseTimerHandler() {
  static bool highPulse = true;
  if (outputQueueCount > 0) {
    if (highPulse) {
      digitalWrite(gamesOutput, LOW);
    } else {
      digitalWrite(gamesOutput, HIGH);
      noInterrupts();
      outputQueueCount--;
      gamesOutputCount++;
      interrupts();
    }
    highPulse = !highPulse;
  } else {
    digitalWrite(gamesOutput, LOW);
  }
}
// ====================================================================

void setup() {
  Serial.begin(115200);
  Serial.println("Starting setup...");

  // Initialize LCD
  lcd.begin(20, 4);
  lcd.setBacklight(255);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Starting setup...");
  lcd.setCursor(0, 1);
  lcd.print("FW: ");
  lcd.print(FIRMWARE_VERSION);

  // Initialize NVS and load settings
  preferences.begin(nvsNamespace, false);

  // #10: boot-loop guard. Counts boots; cleared after the device runs healthy.
  unsigned int bootCount = preferences.getUInt("bootCount", 0) + 1;
  preferences.putUInt("bootCount", bootCount);
  bool bootLoop = (bootCount >= 4);

  deviceID   = preferences.getString("deviceID", "spare");
  ssid       = preferences.getString("ssid", "Yu.me Wifi");
  password   = preferences.getString("password", "");
  ioUsername = preferences.getString("ioUser", ioUsername);
  configPassword   = preferences.getString("cfgPass", "");
  autoUpdateEnabled = preferences.getBool("autoUpd", false);
  otaSecure         = preferences.getBool("otaSecure", false);

  Serial.print("Device ID: ");
  Serial.println(deviceID);

  dollarFeedPath  = ioUsername + "/feeds/" + deviceID + "-dollar-pulses";
  winFeedPath     = ioUsername + "/feeds/" + deviceID + "-win-pulses";
  statusFeedPath  = ioUsername + "/feeds/" + deviceID + "-status";
  commandFeedPath = ioUsername + "/feeds/" + deviceID + "-command";

  mqtt.setServer(AIO_SERVER, AIO_SERVERPORT);
  mqtt.setBufferSize(512);
  mqtt.setSocketTimeout(5);    // #4: bound blocking connect
  mqtt.setKeepAlive(30);
  mqtt.setCallback(mqttCallback);

  // GPIOs
  pinMode(inputDollars, INPUT_PULLUP);
  pinMode(inputWins, INPUT_PULLUP);
  pinMode(resetButton, INPUT_PULLUP);
  pinMode(gamesOutput, OUTPUT);
  digitalWrite(gamesOutput, LOW);
  pinMode(LED_BUILTIN, OUTPUT);

  dollarsStateHigh = digitalRead(inputDollars);
  winsStateHigh = digitalRead(inputWins);
  attachInterrupt(digitalPinToInterrupt(inputDollars), dollarsISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(inputWins), winsISR, CHANGE);

  // Load counters from NVS + seed RAM shadows (#6)
  debounceTime     = preferences.getULong("debounceTime", 60);
  dollarsCount     = preferences.getULong("dollarsCount", 0);
  winsCount        = preferences.getULong("winsCount", 0);
  gamesOutputCount = preferences.getULong("gamesOutputCount", 0);
  savedDollarsCount = dollarsCount;
  savedWinsCount = winsCount;
  savedGamesCount = gamesOutputCount;
  lastSentDollarsCount = dollarsCount;
  lastSentWinsCount = winsCount;

  pulseTimer.attach_ms(100, pulseTimerHandler);

  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);   // #6: we manage creds in NVS; avoid extra flash wear

  // Connect (or recovery AP if a boot loop is detected)
  if (bootLoop) {
    Serial.println("Boot loop detected — starting AP for recovery.");
    updateLCDStatus("Boot loop! AP");
    startAPMode();
  } else {
    connectToWiFi();
  }

  // OTA-success detection (clear flag unconditionally)
  otaJustUpdated = preferences.getBool("otaPending", false);
  if (otaJustUpdated) {
    preferences.putBool("otaPending", false);
    Serial.println("OTA SUCCESS: now running " FIRMWARE_VERSION);
    updateLCDStatus("OTA OK " FIRMWARE_VERSION);
  }

  // One non-blocking MQTT attempt at boot
  if (mqttConnectOnce()) onMqttConnected();

  startWebServer();

  // #4: enable the task watchdog now that boot-time blocking work is done.
  esp_task_wdt_config_t wdtCfg = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  esp_task_wdt_reconfigure(&wdtCfg);
  esp_task_wdt_add(NULL);
  Serial.println("Watchdog armed.");
}

void loop() {
  esp_task_wdt_reset();   // #4: feed the watchdog
  unsigned long currentTime = millis();

  static bool firstLoopRun = true;
  static unsigned long lastReportedDollarsCount = 0;
  static unsigned long lastReportedWinsCount = 0;
  static unsigned long lastSerialPrintTime = 0;

  server.handleClient();
  if (mqtt.connected()) mqtt.loop();

  // #10: once we've run healthy for 30s, mark app valid and clear the boot-loop counter.
  static bool healthMarked = false;
  if (!healthMarked && currentTime > 30000) {
    preferences.putUInt("bootCount", 0);
    esp_ota_mark_app_valid_cancel_rollback();
    healthMarked = true;
    Serial.println("Marked healthy; boot counter cleared.");
  }

  // #4: non-blocking WiFi reconnect (auto-reconnect handles most; re-begin if down)
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiWasConnected) { Serial.println("WiFi disconnected."); wifiWasConnected = false; }
    if (currentTime - lastWifiReconnectAttempt >= wifiReconnectInterval) {
      Serial.println("WiFi re-begin...");
      WiFi.disconnect();
      WiFi.begin(ssid.c_str(), password.c_str());
      lastWifiReconnectAttempt = currentTime;
    }
  } else if (!wifiWasConnected) {
    onWiFiConnected();       // just reconnected — restart mDNS
    wifiWasConnected = true;
  }

  // #4: non-blocking MQTT reconnect (single attempt per interval)
  static unsigned long lastMQTTRetryTime = 0;
  const unsigned long mqttRetryInterval = 15000;
  if (!mqtt.connected() && WiFi.status() == WL_CONNECTED) {
    if (currentTime - lastMQTTRetryTime >= mqttRetryInterval) {
      lastMQTTRetryTime = currentTime;
      if (mqttConnectOnce()) onMqttConnected();
    }
  }

  // #5: periodic auto-update check
  if (autoUpdateEnabled && WiFi.status() == WL_CONNECTED &&
      currentTime - lastAutoUpdateCheck >= autoUpdateInterval) {
    lastAutoUpdateCheck = currentTime;
    checkAutoUpdate(false);
  }

  // Handle ISR flags in the main loop
  if (dollarsISRFlag) {
    noInterrupts();
    if (dollarsStateHigh) { dollarsCount++; outputQueueCount++; }
    dollarsISRFlag = false;
    interrupts();
  }
  if (winsISRFlag) {
    noInterrupts();
    if (winsStateHigh) { winsCount++; outputQueueCount++; }
    winsISRFlag = false;
    interrupts();
  }

  // Reset button long press
  if (digitalRead(resetButton) == LOW) {
    if (!resetPressed) {
      resetPressed = true;
      resetButtonPressTime = currentTime;
    } else if (resetPressed && (currentTime - resetButtonPressTime >= resetHoldTime)) {
      noInterrupts();
      dollarsCount = 0; winsCount = 0; gamesOutputCount = 0; outputQueueCount = 0;
      dollarsStateHigh = digitalRead(inputDollars);
      winsStateHigh = digitalRead(inputWins);
      lastDebounceTimeDollars = micros();
      lastDebounceTimeWins = micros();
      interrupts();
      preferences.putULong("dollarsCount", dollarsCount);
      preferences.putULong("winsCount", winsCount);
      preferences.putULong("gamesOutputCount", gamesOutputCount);
      savedDollarsCount = 0; savedWinsCount = 0; savedGamesCount = 0;
      updateLCDStatus("Reset Done");
      Serial.println("All counters have been reset.");
      resetPressed = false;
    }
  } else {
    resetPressed = false;
  }

  // #6: periodic NVS save using RAM shadows (no per-loop flash reads)
  bool saveRequired = (dollarsCount != savedDollarsCount) ||
                      (winsCount != savedWinsCount) ||
                      (gamesOutputCount != savedGamesCount);
  if (saveRequired) {
    if (changeDetectedTime == 0) changeDetectedTime = currentTime;
    if (currentTime - changeDetectedTime >= 10000) {
      preferences.putULong("dollarsCount", dollarsCount);
      preferences.putULong("winsCount", winsCount);
      preferences.putULong("gamesOutputCount", gamesOutputCount);
      savedDollarsCount = dollarsCount;
      savedWinsCount = winsCount;
      savedGamesCount = gamesOutputCount;
      Serial.println("Counters saved to NVS.");
      changeDetectedTime = 0;
    }
  } else {
    changeDetectedTime = 0;
  }

  // Send pulse data every 10 minutes only if there is a change
  static unsigned long lastSendTime = 0;
  const unsigned long sendDataInterval = 600000;
  bool counterChangedSinceLastSend = (dollarsCount != lastSentDollarsCount) || (winsCount != lastSentWinsCount);
  if (counterChangedSinceLastSend && ((currentTime - lastSendTime >= sendDataInterval) || firstLoopRun)) {
    sendPulseData(true);
    lastSendTime = currentTime;
  }

  // Regular online status every 10 minutes
  static unsigned long lastStatusUpdateTime = 0;
  if (currentTime - lastStatusUpdateTime >= 600000) {
    if (mqtt.connected()) sendStatus();
    lastStatusUpdateTime = currentTime;
  }

  if (firstLoopRun) {
    Serial.print("Initial Counter Status - Dollars: ");
    Serial.print(dollarsCount);
    Serial.print(", Wins: ");
    Serial.println(winsCount);
    lastReportedDollarsCount = dollarsCount;
    lastReportedWinsCount = winsCount;
    lastSerialPrintTime = currentTime;
    firstLoopRun = false;
  }

  // Print counter status only on change or every 10 minutes
  bool countersChanged = (dollarsCount != lastReportedDollarsCount) || (winsCount != lastReportedWinsCount);
  if (countersChanged || (currentTime - lastSerialPrintTime >= 600000)) {
    Serial.print("Counter Status - Dollars: ");
    Serial.print(dollarsCount);
    Serial.print(", Wins: ");
    Serial.println(winsCount);
    lastReportedDollarsCount = dollarsCount;
    lastReportedWinsCount = winsCount;
    lastSerialPrintTime = currentTime;
  }

  // #9: richer LCD, updated every second WITHOUT clear() (no flicker)
  if (currentTime - previousPrintTime >= printInterval) {
    String r0 = "Wins: " + String(winsCount);
    while (r0.length() < 18) r0 += ' ';
    r0 = r0.substring(0, 18) + (mqtt.connected() ? "MQ" : "--");
    lcdPrintRow(0, r0);
    lcdPrintRow(1, "$: " + String(dollarsCount));
    String rssiStr = (WiFi.status() == WL_CONNECTED) ? (String(WiFi.RSSI()) + "dBm") : "no-wifi";
    lcdPrintRow(2, deviceID + " " + rssiStr + " " + uptimeString());
    String ipStr = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : String("AP/--");
    lcdPrintRow(3, String(FIRMWARE_VERSION) + " " + ipStr);
    previousPrintTime = currentTime;
  }
}
