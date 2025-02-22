#include <ETH.h>
#include <WiFi.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#define FORMAT_LITTLEFS_IF_FAILED true

struct Config {  //example config
  char ssid[32];
  char password[64];
};
Config config;  // <- global configuration object

static bool eth_connected = false;

static AsyncWebServer server(80);

// WARNING: onEvent is called from a separate FreeRTOS task (thread)!
void onEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH Started");
      // The hostname must be set after the interface is started, but needs
      // to be set before DHCP, so set it from the event handler thread.
      ETH.setHostname("esp32-ethernet");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED: Serial.println("ETH Connected"); break;
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
    default: break;
  }
}

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

void readSettings() {
  Serial.println("Reading config");
  File file = LittleFS.open("/config.json");
  if (!file) {
    Serial.println("Failed to open config");
    return;
  }
  StaticJsonDocument<512> doc;  // Use StaticJsonDocument with a size
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    Serial.println(F("Failed to read file!!!: "));
    Serial.println(error.c_str());  // Print error details
    file.close();
    return;
  }

  Serial.println("Read Config.");
  strlcpy(config.ssid, doc["ssid"] | "defaultSSID", sizeof(config.ssid));
  strlcpy(config.password, doc["password"] | "defaultPass", sizeof(config.password));

  file.close();
}

void saveConfiguration(const char *filename, const Config &config) {
  // Open file for writing
  File file = LittleFS.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println(F("Failed to create file"));
    return;
  }

  // Allocate a temporary JsonDocument
  JsonDocument doc;

  // Set the values in the document
  //doc["hostname"] = config.hostname;
  //doc["port"] = config.port;

  // Serialize JSON to file
  if (serializeJson(doc, file) == 0) {
    Serial.println(F("Failed to write to file"));
  }

  // Close the file
  file.close();
}

void WIFI_Connect() {
  Serial.print("\nConnecting to ");
  Serial.println(config.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid, config.password);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nboot");
  Network.onEvent(onEvent);
  WiFi.onEvent(WiFiEvent);
  if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED)) {
    Serial.println("LittleFS Mount Failed");
    return;
  }
  readSettings();
  ETH.begin(
    ETH_PHY_LAN8720,      // PHY type (eth_phy_type_t)
    0,                    // PHY address (int32_t)
    23,                   // MDC pin (int)
    18,                   // MDIO pin (int)
    5,                    // Power pin (int)
    ETH_CLOCK_GPIO17_OUT  // Clock mode (eth_clock_mode_t)
  );
  WIFI_Connect();

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

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
          request->send(200, "text/plain", content);  // Send as string
          return;
        } else {
          Serial.println("Failed to open file");
          request->send(404, "text/plain", "File not found");
        }
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
  //saveConfiguration("/config.json", config);
}

void loop() {
  if (eth_connected) {
    Serial.print("online");
  }
  delay(10000);
}
