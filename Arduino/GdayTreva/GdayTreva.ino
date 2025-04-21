//[Ctrl] + [Shift] + [P], then "Upload LittleFS to Pico/ESP8266/ESP32".
#include <ETH.h>
#include <WiFi.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>   // For mDNS support
#include <time.h>      // For time functions
#include <sys/time.h>  // For struct timeval

#define FORMAT_LITTLEFS_IF_FAILED true

struct Config {
  char ssid[32];
  char password[64];
  char mdns_hostname[32];        // Single mDNS hostname for both interfaces
  char ntp_server[32];           // NTP server (e.g., "pool.ntp.org")
  char internet_check_host[32];  // Host to check for internet connectivity (e.g., "8.8.8.8")
};



Config config;  // Global configuration object
static bool eth_connected = false;
static bool wifi_connected = false;
static bool internet_connected = false;
static bool network_up = false;
static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static unsigned long lastInternetCheck = 0;
static unsigned long internetCheckInterval = 60000;
static int internetCheckCount = 0;
unsigned long print_time_count = 0;
const unsigned long ONE_DAY_MS = 24 * 60 * 60 * 1000UL;
unsigned long totalHeap = 0;
int numPrograms = 10;

void networkEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH Started");
      ETH.setHostname(config.mdns_hostname);
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH Connected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.println("ETH Got IP");
      Serial.println("Ethernet IP: " + ETH.localIP().toString());
      eth_connected = true;
      network_up = true;
      //WiFi.disconnect();  // Disconnect WiFi to prioritize Ethernet
      //wifi_connected = false;
      checkInternetConnectivity();
      syncNTPTime();
      if (!MDNS.begin(config.mdns_hostname)) {
        Serial.println("Error setting up mDNS for Ethernet: " + String(config.mdns_hostname));
      } else {
        Serial.println("mDNS started as " + String(config.mdns_hostname) + ".local");
        MDNS.addService("http", "tcp", 80);
        MDNS.addService("ws", "tcp", 80);
      }
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println("ETH Lost IP");
      eth_connected = false;
      network_up = eth_connected || wifi_connected;
      if (!network_up) {
        internet_connected = false;
        MDNS.end();
      }
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("ETH Stopped");
      eth_connected = false;
      network_up = eth_connected || wifi_connected;
      if (!network_up) {
        internet_connected = false;
        MDNS.end();
      }
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("\nWiFi connected.");
      wifi_connected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.print("\nWiFi IP address: ");
      Serial.println(WiFi.localIP().toString());
      wifi_connected = true;
      network_up = true;
      checkInternetConnectivity();
      syncNTPTime();
      if (!MDNS.begin(config.mdns_hostname)) {
        Serial.println("Error setting up mDNS for WiFi: " + String(config.mdns_hostname));
      } else {
        Serial.println("mDNS started as " + String(config.mdns_hostname) + ".local");
        MDNS.addService("http", "tcp", 80);
        MDNS.addService("ws", "tcp", 80);
      }
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      if (wifi_connected) Serial.println("\nWiFi disconnected.");
      wifi_connected = false;
      network_up = eth_connected || wifi_connected;
      if (!network_up) {
        MDNS.end();
        internet_connected = false;
      }
      break;
    default:
      break;
  }
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      break;
    case WS_EVT_DATA:
      {
        Serial.printf("WebSocket client #%u sent data: %s\n", client->id(), (char *)data);
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, (char *)data, len);
        if (!error) {
          const char *command = doc["command"];
          if (command && strcmp(command, "sync_time") == 0) {
            const char *timeStr = doc["time"];
            if (timeStr) {
              setTimeFromClient(timeStr);
            }
          } else if (command && strcmp(command, "get_network_info") == 0) {
            sendNetworkInfo(client);
          } else if (command && strcmp(command, "save_program") == 0) {
            handleSaveProgram(client, doc);
          } else if (strcmp(command, "get_program") == 0) {
            const char *programID = doc["programID"];
            if (programID) handleGetProgram(client, programID);
            else sendErrorResponse(client, "", "Missing programID");
          }
        } else {
          Serial.println("Failed to parse WebSocket JSON: " + String(error.c_str()));
        }
        break;
      }
    case WS_EVT_PONG:
    case WS_EVT_ERROR:
      break;
  }
}

void sendErrorResponse(AsyncWebSocketClient *client, const char *programID, const char *message) {
  StaticJsonDocument<512> response;
  response["type"] = "get_program_response";
  response["success"] = false;
  response["programID"] = programID;
  response["message"] = message;
  String jsonResponse;
  serializeJson(response, jsonResponse);
  client->text(jsonResponse);
  Serial.printf("Sent error to client #%u: %s\n", client->id(), message);
}

String getNextProgramID() {
  for (int i = 1; i <= 10; i++) {
    String idStr = String(i);
    if (i < 10) idStr = "0" + idStr;
    String filename = "/program" + idStr + ".json";
    if (!LittleFS.exists(filename)) {
      return idStr;
    }
  }
  return "";
}

void sendProgramResponse(AsyncWebSocketClient *client, bool success, const char *message, const char *programID) {
  StaticJsonDocument<512> response;
  response["type"] = "save_program_response";
  response["success"] = success;
  response["programID"] = programID;
  response["message"] = message;
  String jsonResponse;
  serializeJson(response, jsonResponse);
  client->text(jsonResponse);
  Serial.printf("Sent save_program_response to client #%u: %s\n", client->id(), message);
}

void handleGetProgram(AsyncWebSocketClient *client, const char *programID) {
  String idStr = String(programID);
  if (idStr.length() != 2 || idStr < "01" || idStr > "10") {
    sendErrorResponse(client, programID, "Invalid programID (must be 01-10)");
    return;
  }

  String filename = "/program" + idStr + ".json";
  if (!LittleFS.exists(filename)) {
    sendErrorResponse(client, programID, "Program not found");
    return;
  }

  File file = LittleFS.open(filename, FILE_READ);
  if (!file) {
    sendErrorResponse(client, programID, "Failed to open program file");
    return;
  }

  String content = file.readString();
  file.close();

  StaticJsonDocument<4096> contentDoc;
  DeserializationError error = deserializeJson(contentDoc, content);
  if (error) {
    sendErrorResponse(client, programID, "Invalid JSON in program file");
    return;
  }

  StaticJsonDocument<512> response;
  response["type"] = "get_program_response";
  response["success"] = true;
  response["programID"] = programID;
  response["content"] = content;
  String jsonResponse;
  serializeJson(response, jsonResponse);
  client->text(jsonResponse);
  Serial.printf("Sent program %s to client #%u\n", programID, client->id());
}

void handleSaveProgram(AsyncWebSocketClient *client, const JsonDocument &doc) {
  const char *programID = doc["programID"];
  const char *content = doc["content"];
  if (!programID || !content) {
    sendProgramResponse(client, false, "Missing programID or content", "");
    return;
  }

  String idStr = String(programID);
  if (idStr.length() != 2 || (idStr != "00" && (idStr < "01" || idStr > "10"))) {
    sendProgramResponse(client, false, "Invalid programID format (must be 00-10)", "");
    return;
  }

  StaticJsonDocument<4096> contentDoc;
  DeserializationError error = deserializeJson(contentDoc, content);
  if (error) {
    sendProgramResponse(client, false, "Invalid JSON content", "");
    return;
  }

  size_t contentSize = measureJson(contentDoc);
  if (contentSize > 4096) {
    sendProgramResponse(client, false, "Program size exceeds 4KB", "");
    return;
  }

  String targetID = idStr;
  if (idStr == "00") {
    targetID = getNextProgramID();
    if (targetID == "") {
      sendProgramResponse(client, false, "No available program slots (max 10)", targetID.c_str());
      return;
    }
  }

  String filename = "/program" + targetID + ".json";
  File file = LittleFS.open(filename, FILE_WRITE);
  if (!file) {
    sendProgramResponse(client, false, "Failed to open file for writing", targetID.c_str());
    return;
  }

  if (serializeJson(contentDoc, file) == 0) {
    file.close();
    sendProgramResponse(client, false, "Failed to write program", targetID.c_str());
    return;
  }

  file.close();
  sendProgramResponse(client, true, "Program saved successfully", targetID.c_str());
}

void sendTimeToClients() {
  if (ws.count() > 0) {
    time_t now = time(nullptr);
    unsigned long freeHeap = ESP.getFreeHeap();
    unsigned long usedHeap = totalHeap - freeHeap;

    String json = "{";
    json += "\"type\":\"time\",";
    json += "\"epoch\":" + String(now) + ",";
    json += "\"mem_used\":" + String(usedHeap) + ",";
    json += "\"mem_total\":" + String(totalHeap);
    json += "}";

    ws.textAll(json);
  }
}

void sendNetworkInfo(AsyncWebSocketClient *client) {
  String wifi_ip = wifi_connected ? WiFi.localIP().toString() : "N/A";
  String eth_ip = eth_connected ? ETH.localIP().toString() : "N/A";
  String wifi_mac = wifi_connected ? WiFi.macAddress() : "N/A";
  String eth_mac = eth_connected ? ETH.macAddress() : "N/A";
  String wifi_gateway = wifi_connected ? WiFi.gatewayIP().toString() : "N/A";
  String eth_gateway = eth_connected ? ETH.gatewayIP().toString() : "N/A";
  String wifi_subnet = wifi_connected ? WiFi.subnetMask().toString() : "N/A";
  String eth_subnet = eth_connected ? ETH.subnetMask().toString() : "N/A";
  IPAddress wifi_dns = wifi_connected ? WiFi.dnsIP() : IPAddress(0, 0, 0, 0);
  String wifi_dns_str = wifi_connected ? wifi_dns.toString() : "N/A";
  IPAddress eth_dns = eth_connected ? ETH.dnsIP() : IPAddress(0, 0, 0, 0);
  String eth_dns_str = eth_connected ? eth_dns.toString() : "N/A";
  int32_t wifi_rssi = wifi_connected ? WiFi.RSSI() : 0;
  String hostname = String(config.mdns_hostname) + ".local";

  StaticJsonDocument<512> doc;
  doc["type"] = "network_info";
  doc["wifi_ip"] = wifi_ip;
  doc["eth_ip"] = eth_ip;
  doc["wifi_mac"] = wifi_mac;
  doc["eth_mac"] = eth_mac;
  doc["wifi_gateway"] = wifi_gateway;
  doc["eth_gateway"] = eth_gateway;
  doc["wifi_subnet"] = wifi_subnet;
  doc["eth_subnet"] = eth_subnet;
  doc["wifi_dns"] = wifi_dns_str;
  doc["eth_dns"] = eth_dns_str;
  doc["mdns_hostname"] = hostname;
  doc["wifi_rssi"] = wifi_rssi;

  String json;
  serializeJson(doc, json);
  client->text(json);
  Serial.println("Sent network info: " + json);
}

bool checkInternetConnectivity() {
  const char *host = config.internet_check_host;
  const int port = 53;
  WiFiClient client;
  Serial.println("Checking internet connectivity to " + String(host) + "...");
  if (client.connect(host, port, 5000)) {
    Serial.println("Internet connection confirmed");
    internet_connected = true;
    internetCheckCount = 0;
    internetCheckInterval = 60000;
    lastInternetCheck = millis();
    client.stop();
    return true;
  } else {
    Serial.println("No internet connection");
    internet_connected = false;
    internetCheckCount++;
    internetCheckInterval = min(internetCheckInterval * 2, ONE_DAY_MS);
    return false;
  }
}

bool syncNTPTime() {
  static bool ntpConfigured = false;
  if (!ntpConfigured && network_up) {
    Serial.println("Configuring NTP with server: " + String(config.ntp_server));
    if (config.ntp_server && strlen(config.ntp_server) > 0) {
      configTime(0, 0, config.ntp_server);
      ntpConfigured = true;
      // Test NTP server reachability
      WiFiUDP udp;
      udp.begin(123);
      if (udp.beginPacket(config.ntp_server, 123)) {
        Serial.println("NTP server reachable");
        udp.endPacket();
      } else {
        Serial.println("Failed to reach NTP server");
      }
      udp.stop();
    } else {
      Serial.println("NTP server invalid, skipping");
      return false;
    }
  }
  return ntpConfigured;
}
void setTimeFromClient(const char *timeStr) {
  if (timeStr) {
    struct tm tm;
    if (sscanf(timeStr, "%d-%d-%d %d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec)
        == 6) {
      tm.tm_year -= 1900;
      tm.tm_mon -= 1;
      tm.tm_isdst = -1;  // No DST, UTC
      time_t t = mktime(&tm);
      if (t != -1) {
        struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        Serial.println("Time set from client (UTC): " + String(timeStr));
      } else {
        Serial.println("Failed to convert time string");
      }
    } else {
      Serial.println("Invalid time format from client");
    }
  }
}

int shouldCheckInternet() {
  if (network_up && !internet_connected) {
    if (millis() - lastInternetCheck >= internetCheckInterval) {
      Serial.println("Retry internet connectivity check triggered (exponential backoff)");
      return 1;
    }
  }
  return 0;
}

void writeFile(const char *path, const char *message) {
  Serial.printf("Writing file: %s\r\n", path);
  File file = LittleFS.open(path, FILE_WRITE);
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
    Serial.println("Failed to open config, using defaults");
    strlcpy(config.ssid, "defaultSSID", sizeof(config.ssid));
    strlcpy(config.password, "defaultPass", sizeof(config.password));
    strlcpy(config.mdns_hostname, "gday", sizeof(config.mdns_hostname));
    strlcpy(config.ntp_server, "pool.ntp.org", sizeof(config.ntp_server));
    strlcpy(config.internet_check_host, "1.1.1.1", sizeof(config.internet_check_host));
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
  strlcpy(config.internet_check_host, doc["internet_check_host"] | "1.1.1.1", sizeof(config.internet_check_host));
  file.close();
}

void saveConfiguration(const char *filename, const Config &config) {
  File file = LittleFS.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println(F("Failed to create file"));
    return;
  }
  StaticJsonDocument<512> doc;
  doc["ssid"] = config.ssid;
  doc["password"] = config.password;
  doc["mdns_hostname"] = config.mdns_hostname;
  doc["ntp_server"] = config.ntp_server;
  doc["internet_check_host"] = config.internet_check_host;
  if (serializeJson(doc, file) == 0) {
    Serial.println(F("Failed to write to file"));
  } else {
    Serial.println(F("Configuration saved"));
  }
  file.close();
}

void WIFI_Connect() {
  Serial.print("\nConnecting to ");
  Serial.println(config.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid, config.password);
  unsigned long startAttemptTime = millis();
  const unsigned long timeout = 10000;  // 10 seconds timeout
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nFailed to connect to WiFi");
  } else {
    Serial.println("\nWiFi connected");
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nboot");
  totalHeap = ESP.getHeapSize();
  Serial.println("Total heap size: " + String(totalHeap) + " bytes");
  WiFi.onEvent(networkEvent);
  if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED)) {
    Serial.println("LittleFS Mount Failed");
    return;
  }
  readSettings();
  pinMode(5, OUTPUT);
  digitalWrite(5, HIGH);
  delay(100);
  if (!ETH.begin(ETH_PHY_LAN8720, 0, 23, 18, 5, ETH_CLOCK_GPIO17_OUT)) {
    Serial.println("Ethernet failed to start");
  }
  WIFI_Connect();
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.onNotFound([](AsyncWebServerRequest *request) {
    // Ignore requests for static assets (e.g., /assets/*)
    if (request->url().startsWith("/assets/")) {
      request->send(404, "text/plain", "Asset not found");
      return;
    }
    // Serve index.html for all other routes
    if (LittleFS.exists("/index.html")) {
      request->send(LittleFS, "/index.html", "text/html");
    } else {
      request->send(404, "text/plain", "Index file not found");
    }
  });


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
  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.begin();
}
bool isTimeSynced() {
  time_t now = time(nullptr);
  return now > 946684800;  // After Jan 1, 2000
}

void loop() {
  static unsigned long lastWiFiRetry = 0;
  if (!wifi_connected && !eth_connected && millis() - lastWiFiRetry >= 30000) {
    WIFI_Connect();
    lastWiFiRetry = millis();
  }
  static unsigned long lastNTPSyncCheck = 0;
  if (millis() - lastNTPSyncCheck >= 30000 && network_up) {
    if (!isTimeSynced()) {
      Serial.println("NTP sync failed, retrying...");
      syncNTPTime();  // Retry NTP sync
    }
    lastNTPSyncCheck = millis();
  }
  if (millis() - print_time_count >= 10000) {
    time_t now = time(nullptr);
    char timeStr[50];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", gmtime(&now));  // UTC time
    Serial.print("UTC Time: ");
    Serial.print(timeStr);
    Serial.print(" WiFi IP address: ");
    Serial.print(WiFi.localIP().toString());
    Serial.println(" Free heap: " + String(ESP.getFreeHeap()));
    print_time_count = millis();
  }
  if (shouldCheckInternet() == 1) {
    checkInternetConnectivity();
  }
  static unsigned long lastTimeSend = 0;
  if (millis() - lastTimeSend > 1000) {
    sendTimeToClients();
    lastTimeSend = millis();
  }
  delay(10);
}