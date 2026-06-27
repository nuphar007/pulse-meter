#include <WiFi.h>
#include <PubSubClient.h>
#include <Ticker.h>
#include <Preferences.h>
#include <LiquidCrystal_PCF8574.h>
#include <WebServer.h>
#include <Wire.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>   // Required for OTA over HTTPS (secureClient)

// Some ESP32 board variants don't define LED_BUILTIN; default to GPIO2 (common onboard LED).
#ifndef LED_BUILTIN
#define LED_BUILTIN 2
#endif

#ifndef FIRMWARE_VERSION
// Local-build fallback shown on the LCD when you compile in the Arduino IDE.
// Bump this to match the release tag you're flashing. CI auto-overrides it with
// the git tag, so OTA builds always show the exact released version.
#define FIRMWARE_VERSION "v1.53"
#endif


// Revision history is tracked via git tags / GitHub Releases:
//   https://github.com/nuphar007/pulse-meter/releases


Preferences preferences; // Create a Preferences object for NVS
Ticker pulseTimer;       // Ticker for handling pulses

// WiFi credentials - now as variables that can be modified
String ssid = "Yu.me Wifi";
String password = "86868686";

// Adafruit IO credentials
String ioUsername = "Yume86";   // Default; loaded from NVS, settable via the web form
// NOTE: the AIO key is never hardcoded. It is entered via the web form and stored
// in NVS (key "aioKey"), then read at runtime in connectToMQTT().
#define AIO_SERVER      "io.adafruit.com"
#define AIO_SERVERPORT  1883 // MQTT port

// Unique Device ID - now loaded from NVS
String deviceID = "spare"; // Default device ID

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

// Serial print interval variables
unsigned long previousPrintTime = 0;   // Tracks the last time Serial printed
const unsigned long printInterval = 1000; // Set interval for Serial print (1 second)

// WiFi reconnection interval
const unsigned long wifiReconnectInterval = 300000; // 5 minutes
unsigned long lastWifiReconnectAttempt = 0;

// Track last sent values for conditional publishing
unsigned long lastSentDollarsCount = 0;
unsigned long lastSentWinsCount = 0;

// Flags for ISR handling
volatile bool dollarsISRFlag = false;
volatile bool winsISRFlag = false;

// Web server
WebServer server(80);

// Topic paths
String dollarFeedPath;
String winFeedPath;
String statusFeedPath;
String commandFeedPath;


void updateLCDStatus(const char* message) {
  lcd.setCursor(0, 3);  // Set cursor to the last row (row index 3)
  const int lcdWidth = 20;
  char formattedMessage[lcdWidth + 1];
  strncpy(formattedMessage, message, lcdWidth);
  formattedMessage[lcdWidth] = '\0';
  lcd.print("                    ");  // Clear the line
  lcd.setCursor(0, 3);
  lcd.print(formattedMessage);
}

void connectToWiFi() {
  updateLCDStatus("WiFi Connect...");
  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid.c_str(), password.c_str());
  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 30000) {
    delay(1000);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    updateLCDStatus("WiFi OK");
    Serial.println("Connected to WiFi.");
    
    // Print IP address to Serial Monitor
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    
    // Start web server in client mode
    server.on("/", handleRoot);
    server.on("/saveSettings", HTTP_POST, handleSaveSettings);
    server.begin();
    Serial.println("HTTP server started in client mode");
  } else {
    Serial.println("Failed to connect to WiFi. Switching to AP mode.");
    startAPMode();
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

  // Start web server in AP mode
  server.on("/", handleRoot);
  server.on("/saveSettings", HTTP_POST, handleSaveSettings);
  server.begin();
  Serial.println("HTTP server started in AP mode");
}

void handleRoot() {
  // Load values from NVS
  String storedSSID = preferences.getString("ssid", ssid);
  String storedPassword = preferences.getString("password", password);
  String storedDeviceID = preferences.getString("deviceID", deviceID);
  String storedIOUser = preferences.getString("ioUser", ioUsername);
  String storedAIOKey = preferences.getString("aioKey", "");
  String commandFeedPath = ioUsername + String("/feeds/") + deviceID + "-command";

  String html = "<html><head>";
  html += "<script>";
  html += "function toggleVisibility(id) {";
  html += "var x = document.getElementById(id);";
  html += "if (x.type === 'password') { x.type = 'text'; } else { x.type = 'password'; }";
  html += "}";
  html += "</script></head><body>";

  html += "<h1>Configuration</h1>";
  html += "<form action=\"/saveSettings\" method=\"post\">";
  html += "Device ID:<br><input type=\"text\" name=\"deviceID\" value=\"" + storedDeviceID + "\"><br>";
  html += "WiFi SSID:<br><input type=\"text\" name=\"ssid\" value=\"" + storedSSID + "\"><br>";

  // WiFi Password with Toggle Button
  html += "WiFi Password:<br>";
  html += "<input type=\"password\" id=\"password\" name=\"password\" value=\"" + storedPassword + "\">";
  html += "<button type=\"button\" onclick=\"toggleVisibility('password')\">Show/Hide</button><br>";

  // Adafruit IO Username field inside the same form
  html += "Adafruit IO Username:<br><input type=\"text\" name=\"ioUser\" value=\"" + storedIOUser + "\"><br>";

  // AIO Key Field inside the same form
  html += "Adafruit IO Key:<br>";
  html += "<input type=\"password\" id=\"aioKey\" name=\"aioKey\" value=\"" + storedAIOKey + "\">";
  html += "<button type=\"button\" onclick=\"toggleVisibility('aioKey')\">Show/Hide</button><br>";

  html += "<input type=\"submit\" value=\"Save\">";
  html += "</form>";

  // --- Counters form ---
  html += "<hr><h2>Set Counters</h2>";
  html += "<form action=\"/setCounters\" method=\"POST\">";
  html += "Dollars: <input type=\"number\" name=\"dollars\" value=\"" + String(dollarsCount) + "\"><br><br>";
  html += "Wins: <input type=\"number\" name=\"wins\" value=\"" + String(winsCount) + "\"><br><br>";
  html += "<input type=\"submit\" value=\"Update Counters\">";
  html += "</form>";

  // --- Debounce form ---
  html += "<hr><h2>Debounce Settings</h2>";
  html += "<form action=\"/setDebounce\" method=\"POST\">";
  html += "Debounce Time (ms): <input type=\"number\" name=\"debounce\" value=\"" + String(debounceTime) + "\"><br><br>";
  html += "<input type=\"submit\" value=\"Update Debounce\">";
  html += "</form>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}


void handleSaveSettings() {
  if (server.hasArg("deviceID") && server.hasArg("ssid") && server.hasArg("password")) {
    // Convert device ID to lowercase for Adafruit IO compatibility
    String newDeviceID = server.arg("deviceID");
    newDeviceID.toLowerCase(); // Convert to lowercase
    
    String newSSID = server.arg("ssid");
    String newPassword = server.arg("password");

    preferences.putString("deviceID", newDeviceID);
    preferences.putString("ssid", newSSID);
    preferences.putString("password", newPassword);
    
  if (server.hasArg("ioUser")) {
    String newIOUser = server.arg("ioUser");
    preferences.putString("ioUser", newIOUser);
    Serial.println("Saved IO Username: " + newIOUser);
  }

  if (server.hasArg("aioKey")) {
    String aioKey = server.arg("aioKey");
    preferences.putString("aioKey", aioKey);
    Serial.println("Saved AIO Key");
  }

    Serial.println("Settings saved:");
    Serial.println("Device ID: " + newDeviceID);
    Serial.println("SSID: " + newSSID);
    Serial.println("Password: " + newPassword);

    // Send response to client before restarting
    server.send(200, "text/html", "<html><body><h2>Settings saved. Rebooting...</h2></body></html>");
    delay(1000); // Allow time for response to be sent before restart

    // Restart device
    ESP.restart();
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void sendPulseData(bool forceSend = false) {
  if (!mqtt.connected()) {
    Serial.println("MQTT not connected. Cannot send data.");
    return;
  }
  



  bool dataChanged = false;
  
  // Check if dollars count has changed or forceSend is true
  if (dollarsCount != lastSentDollarsCount || forceSend) {
    if (!mqtt.publish(dollarFeedPath.c_str(), String(dollarsCount).c_str())) {
      Serial.println("Failed to send dollar pulses");
    } else {
      lastSentDollarsCount = dollarsCount;
      dataChanged = true;
    }
  }

  // Check if wins count has changed or forceSend is true
  if (winsCount != lastSentWinsCount || forceSend) {
    if (!mqtt.publish(winFeedPath.c_str(), String(winsCount).c_str())) {
      Serial.println("Failed to send win pulses");
    } else {
      lastSentWinsCount = winsCount;
      dataChanged = true;
    }
  }

  if (dataChanged || forceSend) {
    Serial.println("Data sent to Adafruit IO: $=" + String(dollarsCount) + ", Wins=" + String(winsCount));
  } else {
    Serial.println("No changes detected. Skipping data send.");
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

  updateLCDStatus("Reset Done");
  Serial.printf("Counters reset (%s).\n", reason);

  sendPulseData(true); // Push reset state to Adafruit IO
}

void sendStatus() {
  if (mqtt.connected()) {
    if (mqtt.publish(statusFeedPath.c_str(), "online")) {
      Serial.println("Status sent: online");
    } else {
      Serial.println("Failed to send status");
    }
  } else {
    Serial.println("MQTT not connected. Cannot send status.");
  }
}

void connectToMQTT() {
  int retryCount = 0;
  const int maxRetries = 3;

  updateLCDStatus("MQTT Connect...");

  // Load AIO Key from NVS
  String aioKey = preferences.getString("aioKey", "");
  if (aioKey == "") {
    Serial.println("No AIO Key found in NVS. MQTT connection aborted.");
    updateLCDStatus("Missing AIO Key");
    return;
  }

  // Check WiFi first
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected. Cannot connect to MQTT.");
    updateLCDStatus("No WiFi");
    return;
  }

  // Create client ID
  char clientID[50];
  snprintf(clientID, sizeof(clientID), "ESP32_%s_%u", deviceID.c_str(), random(0xffff));

  // Try to connect to MQTT server
  while (!mqtt.connected() && retryCount < maxRetries) {
    Serial.print("Connecting to MQTT... ");

    if (mqtt.connect(clientID, ioUsername.c_str(), aioKey.c_str())) {
      updateLCDStatus("MQTT OK");
      Serial.println("Connected!");

      mqtt.subscribe(commandFeedPath.c_str());

      // Immediately send online status
      if (mqtt.publish(statusFeedPath.c_str(), "online")) {
        Serial.println("Status sent: online");
      } else {
        Serial.println("Failed to send status");
      }

      return;  // Success
    } else {
      retryCount++;
      Serial.printf("Failed (%d/%d)\n", retryCount, maxRetries);
      Serial.println("Retrying in 5 seconds...");
      updateLCDStatus("MQTT Retry");
      delay(5000);
    }
  }

  Serial.println("Failed to connect to MQTT after max retries.");
  updateLCDStatus("MQTT Fail");
}


void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.print("MQTT command received [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(message);

  // Ignore self-responses to prevent reprocessing
  if (message.startsWith("RESP:")) {
    return;
  }


  // === Handle OTA Update Command ===
if (message.startsWith("update:")) {
  String url = message.substring(7);
  Serial.println("Starting OTA update from: " + url);

  WiFiClientSecure secureClient;
  secureClient.setInsecure();  // Disable certificate validation

  httpUpdate.rebootOnUpdate(true);
  httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  t_httpUpdate_return ret = httpUpdate.update(secureClient, url);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      Serial.printf("OTA failed: %s\n", httpUpdate.getLastErrorString().c_str());
      break;
    case HTTP_UPDATE_NO_UPDATES:
      Serial.println("OTA: No update available.");
      break;
    case HTTP_UPDATE_OK:
      Serial.println("OTA: Update successful!");
      break;
  }
  return;
}


  // === Handle commands ===
  else if (message.startsWith("set:dollars=")) {
    String val = message.substring(String("set:dollars=").length());
    dollarsCount = val.toInt();
    preferences.putULong("dollarsCount", dollarsCount);
    Serial.printf("MQTT set: dollars = %lu\n", dollarsCount);
    updateLCDStatus("Set $ via MQTT");
    sendPulseData(true);

    // Confirmation
    String response = "RESP: Confirmed: dollars=" + String(dollarsCount);
    mqtt.publish(commandFeedPath.c_str(), response.c_str());
  }

  else if (message.startsWith("set:wins=")) {
    String val = message.substring(String("set:wins=").length());
    winsCount = val.toInt();
    preferences.putULong("winsCount", winsCount);
    Serial.printf("MQTT set: wins = %lu\n", winsCount);
    updateLCDStatus("Set Wins MQTT");
    sendPulseData(true);

    // Confirmation
    String response = "RESP: Confirmed: wins=" + String(winsCount);
    mqtt.publish(commandFeedPath.c_str(), response.c_str());
  }

  else if (message.startsWith("set:debounce=")) {
    String val = message.substring(String("set:debounce=").length());
    debounceTime = val.toInt();
    preferences.putULong("debounceTime", debounceTime);
    Serial.printf("MQTT set: debounce = %lu ms\n", debounceTime);
    updateLCDStatus("Set Debounce");

    // Confirmation
    String response = "RESP: Confirmed: debounce=" + String(debounceTime) + " ms";
    mqtt.publish(commandFeedPath.c_str(), response.c_str());
  }

  else if (message == "on") {
    digitalWrite(LED_BUILTIN, HIGH);
    Serial.println("LED turned ON");
  }

  else if (message == "off") {
    digitalWrite(LED_BUILTIN, LOW);
    Serial.println("LED turned OFF");
  }

  else if (message == "reset") {
    performCounterReset("MQTT");
  }

  else if (message == "status") {
    String statusReport = "RESP: Status OK\n";
    statusReport += "dollars=" + String(dollarsCount) + "\n";
    statusReport += "wins=" + String(winsCount) + "\n";
    statusReport += "debounce=" + String(debounceTime) + " ms\n";
    statusReport += "ip=" + WiFi.localIP().toString() + "\n";
    statusReport +=  "Firmware=" FIRMWARE_VERSION;

    mqtt.publish(commandFeedPath.c_str(), statusReport.c_str());
    Serial.println("Status report sent via MQTT:");
    Serial.println(statusReport);
    updateLCDStatus("Status OK");
  }


  else if (message.startsWith("get:")) {
    String param = message.substring(4);
    String response;

    if (param == "dollars") {
      response = "RESP: Dollars=" + String(dollarsCount);
    } else if (param == "wins") {
      response = "RESP: Wins=" + String(winsCount);
    } else if (param == "debounce") {
      response = "RESP: Debounce=" + String(debounceTime);
    } else {
      response = "RESP: Unknown parameter: " + param;
    }

    mqtt.publish(commandFeedPath.c_str(), response.c_str());
    Serial.println("MQTT get response: " + response);
  }

  else if (message == "?") {
    String helpMessage = "RESP: Available Commands:\n";
    helpMessage += "set:dollars=<val> - Set dollar counter\n";
    helpMessage += "set:wins=<val> - Set win counter\n";
    helpMessage += "set:debounce=<ms> - Set debounce in ms\n";
    helpMessage += "reset - Reset all counters to 0\n";
    helpMessage += "on / off - Turn on/off built-in LED\n";
    helpMessage += "status - Return current status & IP\n";
    helpMessage += "get:dollars - Return dollar counter\n";
    helpMessage += "get:wins - Return win counter\n";
    helpMessage += "get:debounce - Return debounce time";
    helpMessage += "version - version number";

    mqtt.publish(commandFeedPath.c_str(), helpMessage.c_str());
    Serial.println("Help message sent.");
    updateLCDStatus("MQTT Help Sent");
  }

  else if (message == "version") {
    String versionMsg = "RESP: Firmware=" FIRMWARE_VERSION;
    mqtt.publish(commandFeedPath.c_str(), versionMsg.c_str());
    Serial.println("Firmware version sent via MQTT.");
    updateLCDStatus(FIRMWARE_VERSION);
  }


  else {
    Serial.println("Unknown command.");
  }
}



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
  
  // Load device ID and WiFi credentials from NVS
  deviceID = preferences.getString("deviceID", "spare");
  ssid = preferences.getString("ssid", "Yu.me Wifi");
  password = preferences.getString("password", "86868686");
  ioUsername = preferences.getString("ioUser", ioUsername);
  String aioKey = preferences.getString("aioKey", "");
  
  Serial.print("Device ID: ");
  Serial.println(deviceID);
  
  // Create feed paths
  dollarFeedPath = ioUsername + "/feeds/" + deviceID + "-dollar-pulses";
  winFeedPath = ioUsername + "/feeds/" + deviceID + "-win-pulses";
  statusFeedPath = ioUsername + "/feeds/" + deviceID + "-status";
  commandFeedPath = ioUsername + "/feeds/" + deviceID + "-command";

  
  // Configure MQTT server
  mqtt.setServer(AIO_SERVER, AIO_SERVERPORT);
  mqtt.setBufferSize(512);

  // Configure MQTT Callback
  mqtt.setCallback(mqttCallback);

  
  // Print feed paths for debugging
  Serial.println("Dollar feed path: " + dollarFeedPath);
  Serial.println("Win feed path: " + winFeedPath);
  Serial.println("Status feed path: " + statusFeedPath);

  // Initialize GPIOs
  pinMode(inputDollars, INPUT_PULLUP);
  pinMode(inputWins, INPUT_PULLUP);
  pinMode(resetButton, INPUT_PULLUP);  // GPIO 17
  pinMode(gamesOutput, OUTPUT);        // GPIO 16
  digitalWrite(gamesOutput, LOW);

  // Sync the initial state to prevent missed first pulses
  dollarsStateHigh = digitalRead(inputDollars);
  winsStateHigh = digitalRead(inputWins);

  // Attach interrupts for each input with CHANGE mode
  attachInterrupt(digitalPinToInterrupt(inputDollars), dollarsISR, CHANGE);
  attachInterrupt(digitalPinToInterrupt(inputWins), winsISR, CHANGE);

  // Connect to WiFi
  connectToWiFi();

  // Load counter values from NVS
  debounceTime = preferences.getULong("debounceTime", 60);
  dollarsCount = preferences.getULong("dollarsCount", 0);
  winsCount = preferences.getULong("winsCount", 0);
  gamesOutputCount = preferences.getULong("gamesOutputCount", 0);

  // Set up pulse timer for handling output pulses
  pulseTimer.attach_ms(100, pulseTimerHandler);

  // Initialize last sent values
  lastSentDollarsCount = dollarsCount;
  lastSentWinsCount = winsCount;

  // Enable WiFi auto-reconnect
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);

  // Connect to MQTT
  connectToMQTT();

  // Send initial data to Adafruit IO only if connected
  Serial.println("Checking MQTT connection...");
  if (mqtt.connected()) {
    // Send the pulse data
    sendPulseData(true); // Force send initial data
    String bootMsg = "RESP: Booted " FIRMWARE_VERSION;
    mqtt.publish(commandFeedPath.c_str(), bootMsg.c_str());

  } else {
    Serial.println("MQTT not connected. Will send data once connected in main loop.");
  }

    
  // --- Web server routes ---
  server.on("/", HTTP_GET, handleRoot);

  server.on("/setCounters", HTTP_POST, []() {
    if (server.hasArg("dollars") && server.hasArg("wins")) {
      dollarsCount = server.arg("dollars").toInt();
      winsCount = server.arg("wins").toInt();

      // Save to NVS
      preferences.putULong("dollarsCount", dollarsCount);
      preferences.putULong("winsCount", winsCount);

      Serial.printf("Counters updated via web: $=%lu, Wins=%lu\n", dollarsCount, winsCount);
    }

    server.send(200, "text/html", "<p>Counters updated. <a href='/'>Back</a></p>");
  });

  server.on("/setDebounce", HTTP_POST, []() {
    if (server.hasArg("debounce")) {
      debounceTime = server.arg("debounce").toInt();
      preferences.putULong("debounceTime", debounceTime);
      Serial.printf("Debounce time updated: %lu ms\n", debounceTime);
      updateLCDStatus("Debounce OK");
    }

    server.send(200, "text/html", "<p>Debounce updated. <a href='/'>Back</a></p>");
  });


  // Start the web server
  server.begin();
  Serial.println("HTTP server started");



}

void loop() {
  unsigned long currentTime = millis();

  // Initialize static variables for counter reporting
  static bool firstLoopRun = true;
  static unsigned long lastReportedDollarsCount = 0;
  static unsigned long lastReportedWinsCount = 0;
  static unsigned long lastSerialPrintTime = 0;

  // Handle web server requests
  server.handleClient();

  // Process MQTT messages and maintain connection
  if (mqtt.connected()) {
    mqtt.loop();
  }

  // WiFi reconnection handling
  if (WiFi.status() != WL_CONNECTED && (currentTime - lastWifiReconnectAttempt >= wifiReconnectInterval)) {
    Serial.println("WiFi disconnected. Attempting to reconnect...");
    connectToWiFi();
    lastWifiReconnectAttempt = currentTime;
  }

  // MQTT reconnection handling
  static unsigned long lastMQTTRetryTime = 0;
  const unsigned long mqttRetryInterval = 300000; // Try to reconnect every 5 minutes
  static bool reportedDisconnect = false;
  
  if (!mqtt.connected()) {
    if (!reportedDisconnect) {
      Serial.println("MQTT disconnected.");
      reportedDisconnect = true;
    }
    
    if (currentTime - lastMQTTRetryTime >= mqttRetryInterval) {
      Serial.println("Attempting to reconnect to MQTT...");
      connectToMQTT();
      lastMQTTRetryTime = currentTime;
      
      // After reconnecting, try to send data if we're now connected
      if (mqtt.connected()) {
        Serial.println("MQTT reconnection successful. Sending data...");
        
        // Send pulse data
        sendPulseData(true);
        reportedDisconnect = false;
      }
    }
  } else {
    // Only reset flag when we're connected
    reportedDisconnect = false;
  }

  // Handle ISR flags in the main loop
  if (dollarsISRFlag) {
    noInterrupts();
    if (dollarsStateHigh) {
      dollarsCount++;
      outputQueueCount++;
    }
    dollarsISRFlag = false;
    interrupts();
  }

  if (winsISRFlag) {
    noInterrupts();
    if (winsStateHigh) {
      winsCount++;
      outputQueueCount++;
    }
    winsISRFlag = false;
    interrupts();
  }

  // Check for reset button long press
  if (digitalRead(resetButton) == LOW) {
    if (!resetPressed) {
      resetPressed = true;
      resetButtonPressTime = currentTime;
    } else if (resetPressed && (currentTime - resetButtonPressTime >= resetHoldTime)) {
      noInterrupts();
      dollarsCount = 0;
      winsCount = 0;
      gamesOutputCount = 0;
      outputQueueCount = 0;
      dollarsStateHigh = digitalRead(inputDollars);
      winsStateHigh = digitalRead(inputWins);
      lastDebounceTimeDollars = micros(); // Updated to micros()
      lastDebounceTimeWins = micros(); // Updated to micros()
      interrupts();
      preferences.putULong("dollarsCount", dollarsCount);
      preferences.putULong("winsCount", winsCount);
      preferences.putULong("gamesOutputCount", gamesOutputCount);
      updateLCDStatus("Reset Done");
      Serial.println("All counters have been reset.");
      resetPressed = false;
    }
  } else {
    resetPressed = false;
  }

  // Periodic save of counters to NVS with 10-second delay after change
  // Track when a change is first detected for NVS save
  static unsigned long lastSaveTime = 0;
  bool saveRequired = (dollarsCount != preferences.getULong("dollarsCount", 0)) ||
                      (winsCount != preferences.getULong("winsCount", 0)) ||
                      (gamesOutputCount != preferences.getULong("gamesOutputCount", 0));

  if (saveRequired) {
    if (changeDetectedTime == 0) {
      changeDetectedTime = currentTime; // Record the time of the first change
    }

    if (currentTime - changeDetectedTime >= 10000) {
      noInterrupts();
      preferences.putULong("dollarsCount", dollarsCount);
      preferences.putULong("winsCount", winsCount);
      preferences.putULong("gamesOutputCount", gamesOutputCount);
      interrupts();
      Serial.println("Counters saved to NVS.");
      lastSaveTime = currentTime;
      changeDetectedTime = 0; // Reset the change detection timestamp
    }
  } else {
    changeDetectedTime = 0; // Reset the change detection timestamp if no changes are required
  }

  // Send pulse data every 10 minutes only if there is a change
  static unsigned long lastSendTime = 0;
  const unsigned long sendDataInterval = 600000; // 10 minutes
  
  bool counterChangedSinceLastSend = (dollarsCount != lastSentDollarsCount) || (winsCount != lastSentWinsCount);
  bool timeForSend = (currentTime - lastSendTime >= sendDataInterval);
  
  if (counterChangedSinceLastSend && (timeForSend || firstLoopRun)) {
    sendPulseData(true);
    lastSendTime = currentTime;
  }

  // Send regular online status update every 10 minutes
  static unsigned long lastStatusUpdateTime = 0;
  const unsigned long statusUpdateInterval = 600000;  // 10 minutes
  if (currentTime - lastStatusUpdateTime >= statusUpdateInterval) {
    if (mqtt.connected()) {
      sendStatus();
    } else {
      Serial.println("MQTT not connected. Skipping status update.");
    }
    lastStatusUpdateTime = currentTime;
  }

  // Print initial counter values once
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

  // Print current counter statuses only on change or every 10 minutes
  const unsigned long serialPrintInterval = 600000; // 10 minutes (600,000 ms)
  
  bool countersChanged = (dollarsCount != lastReportedDollarsCount) || (winsCount != lastReportedWinsCount);
  bool timeForPrint = (currentTime - lastSerialPrintTime >= serialPrintInterval);
  
  if (countersChanged || timeForPrint) {
    Serial.print("Counter Status - Dollars: ");
    Serial.print(dollarsCount);
    Serial.print(", Wins: ");
    Serial.println(winsCount);
    
    // Update the last reported values
    lastReportedDollarsCount = dollarsCount;
    lastReportedWinsCount = winsCount;
    lastSerialPrintTime = currentTime;
  }

  // Update LCD always (every second)
  if (currentTime - previousPrintTime >= printInterval) {
    // Update LCD
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Wins: ");
    lcd.print(winsCount);
    lcd.setCursor(0, 1);
    lcd.print("$: "); // Changed from "Dollars: " to "$: " for better space usage
    lcd.print(dollarsCount);
    lcd.setCursor(0, 2);
    lcd.print("Device: ");
    lcd.print(deviceID);
    
    // Bottom row: firmware version (always visible) + full IP address
    updateLCDStatus(FIRMWARE_VERSION);
    lcd.setCursor(6, 3); // Position for IP display, shifted left
    lcd.print(WiFi.localIP().toString());
    
    previousPrintTime = currentTime;
  }
}