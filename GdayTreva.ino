#include <ETH.h>
#include <WiFi.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>  // For mDNS support
#include <time.h>     // For time functions
#include <sys/time.h> // For struct timeval

#define FORMAT_LITTLEFS_IF_FAILED true

struct Config {
  char ssid[32];
  char password[64];
  char mdns_hostname[32];  // Single mDNS hostname for both interfaces
  char ntp_server[32];     // NTP server (e.g., "pool.ntp.org")
  char timezone[64];       // Timezone string (e.g., "PST8PDT,M3.2.0,M11.1.0")
  char internet_check_host[32];  // Host to check for internet connectivity (e.g., "8.8.8.8")
};
Config config;  // Global configuration object

static bool eth_connected = false;
static bool wifi_connected = false;  // Track WiFi connection separately
static bool internet_connected = false;  // Flag for internet connectivity
static AsyncWebServer server(80);
static unsigned long lastNTPSync = 0;  // Track last NTP sync time

// Single event handler for all network events (Ethernet and WiFi)
void networkEvent(arduino_event_id_t event) {
  switch (event) {
    // Ethernet events
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH Started");
      ETH.setHostname(config.mdns_hostname);  // Use single hostname for Ethernet
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH Connected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.println("ETH Got IP");
      Serial.println("Ethernet IP: " + ETH.localIP().toString());
      eth_connected = true;
      checkInternetConnectivity();  // Check internet status
      if (!MDNS.begin(config.mdns_hostname)) {  // Use single hostname for mDNS
        Serial.println("Error setting up mDNS for Ethernet: " + String(config.mdns_hostname));
      } else {
        Serial.println("mDNS started as " + String(config.mdns_hostname) + ".local");
        MDNS.addService("http", "tcp", 80);
      }
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println("ETH Lost IP");
      eth_connected = false;
      checkInternetConnectivity();  // Re-check internet status (WiFi might still be connected)
      if (!eth_connected && !wifi_connected) {  // End mDNS only if both interfaces are down
        MDNS.end();  // Clean up mDNS when both interfaces lose IPs
      }
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("ETH Disconnected");
      eth_connected = false;
      checkInternetConnectivity();  // Re-check internet status
      if (!eth_connected && !wifi_connected) {  // End mDNS only if both interfaces are down
        MDNS.end();  // Clean up mDNS when both interfaces disconnect
      }
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("ETH Stopped");
      eth_connected = false;
      checkInternetConnectivity();  // Re-check internet status
      if (!eth_connected && !wifi_connected) {  // End mDNS only if both interfaces are down
        MDNS.end();  // Clean up mDNS when both interfaces stop
      }
      break;
    // WiFi events
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("\nWiFi connected.");
      wifi_connected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("\nWiFi IP address: ");
      Serial.println(WiFi.localIP().toString());
      checkInternetConnectivity();  // Check internet status
      if (!MDNS.begin(config.mdns_hostname)) {  // Use single hostname for mDNS
        Serial.println("Error setting up mDNS for WiFi: " + String(config.mdns_hostname));
      } else {
        Serial.println("mDNS started as " + String(config.mdns_hostname) + ".local");
        MDNS.addService("http", "tcp", 80);
      }
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("\nWiFi disconnected.");
      wifi_connected = false;
      checkInternetConnectivity();  // Re-check internet status (Ethernet might still be connected)
      if (!eth_connected && !wifi_connected) {  // End mDNS only if both interfaces are down
        MDNS.end();  // Clean up mDNS when both interfaces disconnect
      }
      break;
    default:
      break;
  }
}

// Check if the device is connected to the internet
void checkInternetConnectivity() {
  const char* host = config.internet_check_host;  // Use config for internet check host
  const int port = 53;                          // DNS port
  WiFiClient client;

  Serial.println("Checking internet connectivity to " + String(host) + "...");
  if (client.connect(host, port, 5000)) {  // 5-second timeout
    Serial.println("Internet connection confirmed");
    internet_connected = true;
    client.stop();
  } else {
    Serial.println("No internet connection");
    internet_connected = false;
  }
}

// Sync time from NTP server
void syncNTPTime() {
  Serial.println("Syncing time from NTP...");
  configTime(0, 0, config.ntp_server);  // Set timezone offset and NTP server

  // Set timezone
  setenv("TZ", config.timezone, 1);  // Set timezone string (e.g., "PST8PDT,M3.2.0,M11.1.0")
  tzset();

  // Wait for time sync (up to 10 seconds)
  time_t now = time(nullptr);
  int attempts = 0;
  while (now < 1000 && attempts < 10) {  // Ensure time is valid
    delay(1000);
    Serial.print(".");
    now = time(nullptr);
    attempts++;
  }
  if (now >= 1000) {
    Serial.println("\nTime synced: " + String(ctime(&now)));
  } else {
    Serial.println("\nFailed to sync time from NTP");
  }
  lastNTPSync = millis();  // Update last sync time
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
    Serial.println("Failed to open config, using defaults");
    // Set defaults if file is missing
    strlcpy(config.ssid, "defaultSSID", sizeof(config.ssid));
    strlcpy(config.password, "defaultPass", sizeof(config.password));
    strlcpy(config.mdns_hostname, "gday", sizeof(config.mdns_hostname));
    strlcpy(config.ntp_server, "pool.ntp.org", sizeof(config.ntp_server));
    strlcpy(config.timezone, "PST8PDT,M3.2.0,M11.1.0", sizeof(config.timezone));  // Pacific Time as default
    strlcpy(config.internet_check_host, "1.1.1.1", sizeof(config.internet_check_host));  // Default internet check host
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
  strlcpy(config.mdns_hostname, doc["mdns_hostname"] | "gday", sizeof(config.mdns_hostname));
  strlcpy(config.ntp_server, doc["ntp_server"] | "pool.ntp.org", sizeof(config.ntp_server));
  strlcpy(config.timezone, doc["timezone"] | "PST8PDT,M3.2.0,M11.1.0", sizeof(config.timezone));
  strlcpy(config.internet_check_host, doc["internet_check_host"] | "1.1.1.1", sizeof(config.internet_check_host));
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
  doc["ssid"] = config.ssid;
  doc["password"] = config.password;
  doc["mdns_hostname"] = config.mdns_hostname;  // Save single mDNS hostname
  doc["ntp_server"] = config.ntp_server;       // Save NTP server
  doc["timezone"] = config.timezone;           // Save timezone
  doc["internet_check_host"] = config.internet_check_host;  // Save internet check host

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

  // Register a single event handler for all network events (Ethernet and WiFi)
  WiFi.onEvent(networkEvent);

  if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED)) {
    Serial.println("LittleFS Mount Failed");
    return;
  }

  readSettings();

  // Power on LAN8720 PHY before initializing
  pinMode(5, OUTPUT);  // Power pin (GPIO 5)
  digitalWrite(5, HIGH);  // Turn on PHY (adjust if active-low)
  delay(100);  // Allow PHY to stabilize

  // Start Ethernet with your parameters (ESP32 Dev Module + LAN8720)
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
  if (internet_connected && (millis() - lastNTPSync > 24 * 60 * 60 * 1000)) {  // Sync every 24 hours
    syncNTPTime();
  }
  if (eth_connected && internet_connected) {
    Serial.println("Ethernet online with internet");
  } else if (internet_connected) {
    Serial.println("WiFi online with internet");
  } else if (eth_connected) {
    Serial.println("Ethernet online (no internet)");
  } else {
    Serial.println("WiFi online (no internet)");
  }
  delay(10000);
}