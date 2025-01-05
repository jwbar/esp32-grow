#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <UniversalTelegramBot.h>
#include <ArduinoOTA.h>
#include <FS.h>
#include <SPIFFS.h> // Include SPIFFS library
#include <time.h>

// Set your time zone (for example, UTC+2 for Central European Time with daylight savings)
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;    // Adjust for your timezone (3600 = GMT+1, 7200 = GMT+2, etc.)
const int daylightOffset_sec = 3600; // Daylight savings time offset

String getCurrentTime() {
  time_t now = time(nullptr);
  struct tm *currentTime = localtime(&now);
  
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%H:%M:%S", currentTime);
  
  return String(buffer);
}

// WiFi credentials
const char* ssid = "katarifarms";
const char* password = "microgreens";

// Telegram bot credentials
String botToken = "7832524673:AAG3U7UfUymdGU2sE1c0cgLtbWVSHtvNxmY";
String chatID = "6732532260";

// DHT11 sensor setup
#define DHTPIN 21
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// Relay pins
const int relayPins[] = { 26, 27, 4, 16, 17, 5, 18, 19 };
const int relayCount = 8;

// Timing for relays
unsigned long lastTempCheck = 0;
const unsigned long tempInterval = 3600000; // 1 hour in milliseconds
unsigned long lastRelayCheck = 0;
unsigned long lastRelay6Toggle = 0;
const unsigned long relay6Interval = 7200000; // 2 hours in milliseconds

// Web server
AsyncWebServer server(80);

// Telegram bot instance
WiFiClientSecure client;
UniversalTelegramBot bot(botToken, client);

// Relay names
const char* relayNames[] = {"Microgreens1", "Microgreens2", "Microgreens3", "Spirulina Lamp", "HydroPump", "HydroLamp", "Heating", "Relay8"};

// Wi-Fi connection function
void connectToWiFi() {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to Wi-Fi...");
  }
  Serial.println("Connected to Wi-Fi");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); 
}

// SPIFFS initialization
void setupSPIFFS() {
  if (!SPIFFS.begin(true)) {
    Serial.println("An error occurred while mounting SPIFFS");
    return;
  }
}

// Setup the relays as outputs and turn them off
void setupRelays() {
  for (int i = 0; i < relayCount; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], HIGH); // Assume LOW is ON for relays
  }
}

// Handle relay control logic based on time
void handleRelayControl() {
  time_t now = time(nullptr);
  struct tm *currentTime = localtime(&now);
  int currentHour = currentTime->tm_hour;

  // Debugging
  Serial.print("Current Hour: ");
  Serial.println(currentHour);

  // Turn on relays between 19:00 and 07:00
  if ((currentHour >= 19) || (currentHour < 7)) {
    for (int i = 0; i < 4; i++) {
      digitalWrite(relayPins[i], LOW); // Turn on relays
      Serial.print(relayNames[i]);
      Serial.println(" turned ON");
    }
    sendTelegramMessage("Lights turned on.");
  } else {
    for (int i = 0; i < 4; i++) {
      digitalWrite(relayPins[i], HIGH); // Turn off relays
      Serial.print(relayNames[i]);
      Serial.println(" turned OFF");
    }
    sendTelegramMessage("Lights turned off.");
  }

  // Relay 5 (fan) is always on
  digitalWrite(relayPins[4], LOW);

  

  // Control relay 7 (heating) based on temperature
  float temperature = dht.readTemperature();
  if (temperature < 14) {
    digitalWrite(relayPins[6], LOW); // Turn on relay 7 (heating)
    sendTelegramMessage("Heating turned on.");
  } else if (temperature >= 18) {
    digitalWrite(relayPins[6], HIGH); // Turn off relay 7
    sendTelegramMessage("Heating turned off.");
  }
}


// Send JSON data to remote server
void sendJSONData(float temp, float humidity) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin("http://mir1.hopto.org:5001/kcontainer");

    StaticJsonDocument<200> doc;
    doc["temperature"] = temp;
    doc["humidity"] = humidity;
    String jsonData;
    serializeJson(doc, jsonData);

    // Add headers
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Content-Length", String(jsonData.length()));

    // Send POST request
    int httpResponseCode = http.POST(jsonData);
    String response = http.getString();
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    Serial.print("Response: ");
    Serial.println(response);

    http.end();
  } else {
    Serial.println("Wi-Fi not connected. Failed to send JSON data.");
  }
}

// Write JSON file to SPIFFS
void updateJSONFile() {
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();

  StaticJsonDocument<256> jsonDoc;
  jsonDoc["timestamp"] = time(nullptr);
  jsonDoc["temperature"] = temperature;
  jsonDoc["humidity"] = humidity;

  JsonObject relayStatus = jsonDoc.createNestedObject("relay_status");
  for (int i = 0; i < relayCount; i++) {
    relayStatus["relay_" + String(i + 1)] = (digitalRead(relayPins[i]) == LOW) ? "on" : "off";
  }

  File file = SPIFFS.open("/kcontainer.json", FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open kcontainer.json file for writing");
    return;
  }

  if (serializeJson(jsonDoc, file) == 0) {
    Serial.println("Failed to write JSON data to file");
  } else {
    Serial.println("JSON data written to file successfully");
  }
  file.close();
}

// Send a message via Telegram
void sendTelegramMessage(String message) {
  bot.sendMessage(chatID, message, "");
}

// Handle incoming Telegram commands
void handleTelegramCommands() {
  int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

  for (int i = 0; i < numNewMessages; i++) {
    String text = bot.messages[i].text;
    String from = bot.messages[i].from_name;

    // Check if the message is a command to turn a relay on/off
    if (text.startsWith("/turnrelay")) {
      int relayNum = text.substring(11).toInt();
      String action = text.substring(13 + String(relayNum).length());

      if (relayNum >= 1 && relayNum <= relayCount) {
        digitalWrite(relayPins[relayNum - 1], (action == "on") ? LOW : HIGH);
        String state = (action == "on") ? "on" : "off";
        sendTelegramMessage("Relay " + String(relayNum) + " turned " + state + " by " + from);
      }
    }
  }
}

// Start web server and relay control interface
void startWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    String htmlPage = "<!DOCTYPE html><html lang='en'><head>";
    htmlPage += "<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    htmlPage += "<title>Katari Farms Status</title>";

    // CSS styles
    htmlPage += "<style>";
    htmlPage += "body { background-color: #1a1a1a; color: #f1f1f1; font-family: 'Courier New', Courier, monospace; text-align: center; margin: 0; padding: 0; display: flex; flex-direction: column; justify-content: center; height: 100vh; }";
    htmlPage += "h1, h2 { color: #27ae60; margin-bottom: 20px; }";
    htmlPage += ".ascii-art { font-size: 12px; line-height: 1; margin-bottom: 20px; }";
    htmlPage += ".details { font-size: 16px; color: #7f8c8d; }";
    htmlPage += ".data-container { display: flex; flex-direction: column; justify-content: center; align-items: center; margin-top: 20px; }";
    htmlPage += ".status-container { font-size: 14px; border: 1px solid #f1f1f1; padding: 10px; width: 45%; margin: 10px; }";
    htmlPage += ".relay-button { margin: 5px; padding: 10px; background-color: #27ae60; border: none; color: white; cursor: pointer; }";
    htmlPage += ".relay-button:hover { background-color: #219653; }";
    htmlPage += "@media (min-width: 768px) { .data-container { flex-direction: row; flex-wrap: wrap; justify-content: center; } .status-container { width: 30%; } }";
    htmlPage += "</style>";

    htmlPage += "</head><body><div class='ascii-art'>";
    htmlPage += "<h1>Katari Farms</h1><h2>System Status Page</h2>";
    htmlPage += "</div>";

    htmlPage += "<div class='data-container'>";

    // Show temperature and humidity
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();

    htmlPage += "<div class='status-container'>";
    htmlPage += "<p>Current Time: " + getCurrentTime() + "</p>";
    htmlPage += "<p>Temperature: " + String(temperature) + "°C</p>";
    htmlPage += "<p>Humidity: " + String(humidity) + "%</p>";
    htmlPage += "</div>";

    // Show relay statuses
for (int i = 0; i < relayCount; i++) {
  String relayState = (digitalRead(relayPins[i]) == LOW) ? "ON" : "OFF";
  htmlPage += "<div class='status-container'>";
  htmlPage += "<p>" + String(relayNames[i]) + ": " + relayState + "</p>";
  htmlPage += "<button class='relay-button' onclick=\"location.href='/relay/on?relay=" + String(i + 1) + "'\">Turn ON</button>";
  htmlPage += "<button class='relay-button' onclick=\"location.href='/relay/off?relay=" + String(i + 1) + "'\">Turn OFF</button>";
  htmlPage += "</div>";
}

    htmlPage += "</div></body></html>";
    request->send(200, "text/html", htmlPage);
  });

 // Handle relay control via web interface (turn relay ON)
server.on("/relay/on", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("relay")) {
      String relayParam = request->getParam("relay")->value();
      int relayIndex = relayParam.toInt() - 1;

      if (relayIndex >= 0 && relayIndex < relayCount) {
        digitalWrite(relayPins[relayIndex], LOW); // Turn relay ON
        request->send(200, "text/plain", "Relay " + String(relayIndex + 1) + " turned ON");
      } else {
        request->send(400, "text/plain", "Invalid relay index");
      }
    } else {
      request->send(400, "text/plain", "Relay parameter missing");
    }
});

// Handle relay control via web interface (turn relay OFF)
server.on("/relay/off", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("relay")) {
      String relayParam = request->getParam("relay")->value();
      int relayIndex = relayParam.toInt() - 1;

      if (relayIndex >= 0 && relayIndex < relayCount) {
        digitalWrite(relayPins[relayIndex], HIGH); // Turn relay OFF
        request->send(200, "text/plain", "Relay " + String(relayIndex + 1) + " turned OFF");
      } else {
        request->send(400, "text/plain", "Invalid relay index");
      }
    } else {
      request->send(400, "text/plain", "Relay parameter missing");
    }
});

  // Start server
  server.begin();
}

// Setup function
void setup() {
  Serial.begin(115200);

  connectToWiFi();
  setupSPIFFS();
  setupRelays();

  // Initialize DHT sensor
  dht.begin();

  // Initialize OTA
  ArduinoOTA.begin();

  // Start web server
  startWebServer();
}

// Main loop
void loop() {
  ArduinoOTA.handle();
  handleTelegramCommands();
  handleRelayControl();

  // Check temperature and humidity every hour
  if (millis() - lastTempCheck > tempInterval) {
    lastTempCheck = millis();
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();
    sendJSONData(temperature, humidity);
    updateJSONFile();
  }
}
