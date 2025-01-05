#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <DHT.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <UniversalTelegramBot.h>
#include <ArduinoOTA.h>
#include <FS.h>
#include <SPIFFS.h>
#include <time.h>
#include <NTPClient.h>

// Time and network setup
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3600;   
const int daylightOffset_sec = 3600; 

// WiFi credentials
const char* ssid = "ZELDAIRIS";
const char* password = "K4r4m3ll0";

// Telegram bot credentials
String botToken = "7536315323:AAFsYEuyhDIv6_UP_8DhbDrUtks9gcNwm_Q";
String chatID = "6732532260";

// NTP and DHT setup
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntpServer, gmtOffset_sec + daylightOffset_sec);
#define DHTPIN 13
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// Soil moisture sensors and relay pins
const int soilPin1 = 34;
const int soilPin2 = 35;
const int relayPins[] = { 32, 33, 27 };
const int relayCount = 3;
const char* relayNames[] = { "Lights", "Fan", "Pump" };

unsigned long lastTempCheck = 0;
const unsigned long tempInterval = 600000; 
bool nightCycleActive = false;

AsyncWebServer server(80);
WiFiClientSecure client;
UniversalTelegramBot bot(botToken, client);

// Function declarations
void connectToWiFi();
void setupSPIFFS();
void startWebServer();
void sendTelegramMessage(String message);
void handleRelayControl();
bool isInTimeRange(int startHour, int endHour);
void controlRelay3();
String getCurrentTime();
void sendJSONData(float temp, float humidity);
void updateJSONFile();
void pushJSONToServer();

void setup() {
    Serial.begin(115200);
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS mount failed");
        return;
    }
    for (int i = 0; i < relayCount; i++) {
        pinMode(relayPins[i], OUTPUT);
        digitalWrite(relayPins[i], HIGH);
    }
    dht.begin();
    connectToWiFi();
    timeClient.begin(); 
    startWebServer();
    sendTelegramMessage("VeggieBox is now online and monitoring relays and temperature.");
}

// Connect to WiFi
void connectToWiFi() {
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Connecting to Wi-Fi...");
  }
  Serial.println("Connected to Wi-Fi");
}

// Send a message through Telegram
void sendTelegramMessage(String message) {
  if (bot.sendMessage(chatID, message, "")) {
    Serial.println("Telegram message sent: " + message);
  } else {
    Serial.println("Error sending Telegram message");
  }
}

// HTML and CSS to display page on server
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

    // Show temperature, humidity, and soil moisture levels
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();
    int soil1 = analogRead(soilPin1);
    int soil2 = analogRead(soilPin2);

    htmlPage += "<div class='status-container'>";
    htmlPage += "<p>Current Time: " + getCurrentTime() + "</p>";
    htmlPage += "<p>Temperature: " + String(temperature) + "°C</p>";
    htmlPage += "<p>Humidity: " + String(humidity) + "%</p>";
    htmlPage += "<p>Soil Moisture 1: " + String(soil1) + "</p>";
    htmlPage += "<p>Soil Moisture 2: " + String(soil2) + "</p>";
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

  // Relay control (turn relay ON)
  server.on("/relay/on", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("relay")) {
      String relayParam = request->getParam("relay")->value();
      int relayIndex = relayParam.toInt() - 1;

      if (relayIndex >= 0 && relayIndex < relayCount) {
        digitalWrite(relayPins[relayIndex], LOW); 
        request->send(200, "text/plain", "Relay " + String(relayIndex + 1) + " turned ON");
      } else {
        request->send(400, "text/plain", "Invalid relay index");
      }
    } else {
      request->send(400, "text/plain", "Relay parameter missing");
    }
  });

  // Relay control (turn relay OFF)
  server.on("/relay/off", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("relay")) {
      String relayParam = request->getParam("relay")->value();
      int relayIndex = relayParam.toInt() - 1;

      if (relayIndex >= 0 && relayIndex < relayCount) {
        digitalWrite(relayPins[relayIndex], HIGH);
        request->send(200, "text/plain", "Relay " + String(relayIndex + 1) + " turned OFF");
      } else {
        request->send(400, "text/plain", "Invalid relay index");
      }
    } else {
      request->send(400, "text/plain", "Relay parameter missing");
    }
  });
  server.begin();
}

// Get the current time as a string
String getCurrentTime() {
  timeClient.update();
  return timeClient.getFormattedTime();
}

// Relay control based on conditions
void handleRelayControl() {
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  int currentHour = getCurrentTime().substring(0, 2).toInt();

  bool nightCycle = (currentHour >= 17 || currentHour < 11);
  digitalWrite(relayPins[0], nightCycle ? LOW : HIGH); 
  digitalWrite(relayPins[1], nightCycle ? LOW : HIGH);

  if (nightCycle && !nightCycleActive) {
    sendTelegramMessage("Night cycle ON.");
    nightCycleActive = true;
  } else if (!nightCycle && nightCycleActive) {
    sendTelegramMessage("Night cycle OFF.");
    nightCycleActive = false;
  }
}

// Relay 3 control for watering plants based on soil data
void controlRelay3() {
  int soil1 = analogRead(soilPin1);
  int soil2 = analogRead(soilPin2);
  int avgSoil = (soil1 + soil2) / 2;

  static bool relay3Active = false;
  if (avgSoil > 2500 && !relay3Active) {
    relay3Active = true;
    digitalWrite(relayPins[2], LOW); 
    sendTelegramMessage("Relay 3 ON for watering plants.");
  } else if (relay3Active && avgSoil < 2000) {
    relay3Active = false;
    digitalWrite(relayPins[2], HIGH);
    sendTelegramMessage("Relay 3 OFF after watering.");
  }
}

// Push veggibox.json file to server
void pushJSONToServer() {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin("http://192.168.2.103:5002/veggibox");

        File file = SPIFFS.open("/veggibox.json", "r");
        if (file) {
            String jsonData = file.readString();
            file.close();

            http.addHeader("Content-Type", "application/json");
            int httpResponseCode = http.POST(jsonData);

            Serial.print("HTTP Response code: ");
            Serial.println(httpResponseCode);
            http.end();
        }
    } else {
        Serial.println("Wi-Fi not connected. Failed to send JSON data.");
    }
}

// Update JSON file in SPIFFS
void updateJSONFile() {
  float temperature = dht.readTemperature();
  float humidity = dht.readHumidity();
  int soil1 = analogRead(soilPin1);
  int soil2 = analogRead(soilPin2);

  StaticJsonDocument<256> jsonDoc;
  jsonDoc["timestamp"] = getCurrentTime();
  jsonDoc["temperature"] = temperature;
  jsonDoc["humidity"] = humidity;
  jsonDoc["soil1"] = soil1;
  jsonDoc["soil2"] = soil2;

  JsonObject relayStatus = jsonDoc.createNestedObject("relay_status");
  for (int i = 0; i < relayCount; i++) {
    relayStatus["relay_" + String(i + 1)] = (digitalRead(relayPins[i]) == LOW) ? "on" : "off";
  }

  File file = SPIFFS.open("/veggibox.json", FILE_WRITE);
  if (!file) {
    Serial.println("Failed to open json file for writing");
    return;
  }
  serializeJson(jsonDoc, file);
  file.close();
}

void loop() {
  handleRelayControl();
  controlRelay3();
  updateJSONFile();
  pushJSONToServer(); // Push the JSON file to the server after each update
  delay(60000); // Delay to avoid excessive server requests
}
