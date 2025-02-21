#ifndef ETH_PHY_TYPE
#define ETH_PHY_TYPE  ETH_PHY_LAN8720
#define ETH_PHY_ADDR  0
#define ETH_PHY_MDC   23
#define ETH_PHY_MDIO  18
#define ETH_PHY_POWER 5
#define ETH_CLK_MODE  ETH_CLOCK_GPIO17_OUT
#endif

#include <ETH.h>
#include <WiFi.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

#define FORMAT_LITTLEFS_IF_FAILED true

struct Config { //example config
  char hostname[64];
  int port;
};
Config config;                         // <- global configuration object

const char *ssid = "SpaceBucks";
const char *password = "EXCLAIM107";
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
  Serial.println("Reading file: config.json");

  File file = LittleFS.open("/config.json");
  if (!file) {
    Serial.println("Failed to open config");
    writeFile(LittleFS, "/config.json", "this is a config");
    return;
  }
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  if (error)
    Serial.println(F("Failed to read file, using default configuration"));

  Serial.println("Read Config.");
  while (file.available()) {
    config.port = doc["port"] | 2731;
    strlcpy(config.hostname,                  // <- destination
      doc["hostname"] | "example.com",  // <- source
      sizeof(config.hostname));         // <- destination's capacity

    Serial.write(file.read());
  }
  file.close();
}

void saveConfiguration(const char* filename, const Config& config) {
  // Open file for writing
  File file = LittleFS.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println(F("Failed to create file"));
    return;
  }

  // Allocate a temporary JsonDocument
  JsonDocument doc;

  // Set the values in the document
  doc["hostname"] = config.hostname;
  doc["port"] = config.port;

  // Serialize JSON to file
  if (serializeJson(doc, file) == 0) {
    Serial.println(F("Failed to write to file"));
  }

  // Close the file
  file.close();
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nboot");
  Network.onEvent(onEvent);
  ETH.begin();
  WiFi.begin(ssid, password); 

if(!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED)){
  Serial.println("LittleFS Mount Failed");
  return;
  }

  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.begin();
  readSettings();
  saveConfiguration("/config.json", config);
}

void loop() {
  if (eth_connected) {
    Serial.print("online");
  }
  delay(10000);
}
