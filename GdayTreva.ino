#include <ETH.h>
#include <WiFi.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#define FORMAT_LITTLEFS_IF_FAILED true

struct Config {
  char ssid[32];
  char password[64];
};
Config config;  // Global configuration object

static bool eth_connected = false;
static AsyncWebServer server(80);

// Event handler for Ethernet
void onEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH Started");
      ETH.setHostname("esp32-ethernet");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH Connected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.println("ETH Got IP");
      Serial.println(ETH);
      eth_connected = true;
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println("ETH Lost IP");
      eth_connected = false;
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("ETH Disconnected");
      eth_connected = false;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("ETH Stopped");
      eth_connected = false;
      break;
    default:
      break;
  }
}

// Event handler for WiFi
void WiFiEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("\nWiFi connected.");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("\nWiFi IP address: ");
      Serial.println(WiFi.localIP());
      break;
    default:
      break;
  }
}

// Write to LittleFS
void writeFile(fs::FS &fs, const char *path, const char *message) {
  Serial.printf("Writing file: %s\r\n", path);
  File file = fs.open(path, FILE_WRITE);
  if (!file) {
    Serial.println("- failed to open file for writing");
    return;
  }
  if (file.print(message)) {
    Serial.println("- file written");
  } else {
    Serial.println("- write failed");
  }
  file.close();
}

// Read configuration from LittleFS
void readSettings() {
  Serial.println("Reading config");
  File file = LittleFS.open("/config.json");
  if (!file) {
    Serial.println("Failed to open config");
    return;
  }
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    Serial.print(F("Failed to read file!!!: "));
    Serial.println(error.c_str());
    file.close();
    return;
  }
  Serial.println("Read Config.");
  strlcpy(config.ssid, doc["ssid"] | "defaultSSID", sizeof(config.ssid));
  strlcpy(config.password, doc["password"] | "defaultPass", sizeof(config.password));
  file.close();
}

// Save configuration to LittleFS
void saveConfiguration(const char *filename, const Config &config) {
  File file = LittleFS.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println(F("Failed to create file"));
    return;
  }
  StaticJsonDocument<512> doc;
  doc["ssid"] = config.ssid;        // Save SSID
  doc["password"] = config.password; // Save password

  if (serializeJson(doc, file) == 0) {
    Serial.println(F("Failed to write to file"));
  } else {
    Serial.println(F("Configuration saved"));
  }
  file.close();
}

// Connect to WiFi
void WIFI_Connect() {
  Serial.print("\nConnecting to ");
  Serial.println(config.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid, config.password);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nboot");

  // Assuming Network.onEvent should be ETH.onEvent
  Network.onEvent(onEvent);  // Register Ethernet event handler
  WiFi.onEvent(WiFiEvent); // Register WiFi event handler

  if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED)) {
    Serial.println("LittleFS Mount Failed");
    return;
  }

  readSettings();

  // Start Ethernet with explicit parameters
  if (!ETH.begin(ETH_PHY_LAN8720, 0, 23, 18, 5, ETH_CLOCK_GPIO17_OUT)) {
    Serial.println("Ethernet failed to start");
  }

  WIFI_Connect();

  // Serve static files from LittleFS
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // Handle /getFile endpoint
  server.on("/getFile", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("filename")) {
      String filename = request->getParam("filename")->value();
      Serial.println("Requested file: /" + filename);
      if (LittleFS.exists("/" + filename)) {
        Serial.println("File exists");
        File file = LittleFS.open("/" + filename, "r");
        if (file) {
          Serial.println("File opened, size: " + String(file.size()));
          String content = file.readString();
          Serial.println("File content: " + content);
          Serial.println("Free heap: " + String(ESP.getFreeHeap()));
          file.close();
          request->send(200, "text/plain", content);
          return;
        }
        Serial.println("Failed to open file");
        request->send(404, "text/plain", "File not found");
      } else {
        Serial.println("File does not exist");
        request->send(404, "text/plain", "File not found");
      }
    } else {
      Serial.println("No filename provided");
      request->send(400, "text/plain", "Missing filename parameter");
    }
  });

  server.begin();
  // Uncomment to save config if needed
  // saveConfiguration("/config.json", config);
}

void loop() {
  if (eth_connected) {
    Serial.println("Ethernet online");
  }
  delay(10000);
}