#include <ETH.h>
#include <WiFi.h>
#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <time.h>
#include <sys/time.h>
#include <ElegantOTA.h>
#include <map>     // For subscriber_epochs
#include <vector>  // For null_program_ids and trigger_info

#define FORMAT_LITTLEFS_IF_FAILED true

// Pin definitions for outputs A and B
#define OUTPUT_A_PIN 4
#define OUTPUT_B_PIN 12

// OTA Credentials
const char *OTA_USERNAME = "admin";
const char *OTA_PASSWORD = "admin";

// OTA Progress Tracking
static unsigned long otaProgressMillis = 0;

struct Config {
  char ssid[32];
  char password[64];
  char mdns_hostname[32];
  char ntp_server[32];
  char internet_check_host[32];
  int time_offset_minutes;
};
Config config;

struct CycleConfig {
  int runSeconds = 0;
  int stopSeconds = 0;
  bool startHigh = false;
  bool valid = false;
};
CycleConfig cycleConfigA, cycleConfigB;
int activeProgramA = -1, activeProgramB = -1;

std::vector<int> null_program_ids;

struct ProgramConfig {
  int id = -1;                // Program ID (1-10, -1 if invalid)
  String output = "A";        // Output ("A", "B", or "Null")
  String trigger = "Manual";  // Trigger ("Manual", "Cycle Timer", etc.)
  bool active = false;        // Whether the program is active (from isProgramActive)
};
ProgramConfig programConfigs[11];

// Cycle Timer state for each output
struct CycleState {
  bool isRunning;
  unsigned long lastSwitchTime;
  bool isOnPhase;
  int activeProgram;
  time_t nextToggle;
};
CycleState cycleStateA = { false, 0, false, -1 };
CycleState cycleStateB = { false, 0, false, -1 };

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
bool programsChanged = true;
bool outputAState = false, outputBState = false;

// Subscriber map and status tracking
std::map<AsyncWebSocketClient *, uint32_t> subscriber_epochs;
uint32_t last_output_epoch = 0;
std::vector<int> last_null_program_ids;

// Trigger info struct
struct TriggerInfo {
  int id;
  String output;
  String trigger;
  time_t next_toggle;
  String sensor_value;
  bool state;
  bool operator==(const TriggerInfo &other) const {
    return id == other.id && output == other.output && trigger == other.trigger && next_toggle == other.next_toggle && sensor_value == other.sensor_value && state == other.state;
  }
};
std::vector<TriggerInfo> last_trigger_info;

// Sends output status to subscribed clients
void sendOutputStatus(const std::vector<int> &null_program_ids) {
  if (subscriber_epochs.empty()) return;

  uint32_t new_epoch = millis();
  StaticJsonDocument<512> doc;
  doc["type"] = "output_status";
  doc["epoch"] = new_epoch;
  doc["prog_a"] = cycleStateA.activeProgram;
  doc["prog_b"] = cycleStateB.activeProgram;
  doc["out_a"] = outputAState;
  doc["out_b"] = outputBState;

  JsonArray null_ids = doc.createNestedArray("null_progs");
  for (int id : null_program_ids) {
    null_ids.add(id);
  }

  String json;
  serializeJson(doc, json);

  for (auto &pair : subscriber_epochs) {
    if (new_epoch > pair.second) {
      pair.first->text(json);
      pair.second = new_epoch;
      Serial.printf("Sent output_status to client #%u\n", pair.first->id());
    }
  }
  last_output_epoch = new_epoch;
}

// Sends trigger status to subscribed clients
void sendTriggerStatus(const std::vector<TriggerInfo> &trigger_info) {
  if (subscriber_epochs.empty()) return;

  uint32_t new_epoch = millis();
  StaticJsonDocument<512> doc;
  doc["type"] = "trigger_status";
  doc["epoch"] = new_epoch;

  JsonArray progs = doc.createNestedArray("progs");
  for (const auto &info : trigger_info) {
    JsonObject prog = progs.createNestedObject();
    prog["id"] = info.id;
    prog["output"] = info.output;
    prog["trigger"] = info.trigger;
    if (info.output == "A" || info.output == "B") {
      prog["state"] = info.state;  // Add state for A/B outputs
    }
    if ((info.trigger == "Cycle Timer") && info.next_toggle > 0) {
      prog["next_toggle"] = info.next_toggle;
    } else if (info.output == "Null" && info.trigger != "Manual" && !info.sensor_value.isEmpty()) {
      prog["value"] = info.sensor_value;
    }
  }

  String json;
  serializeJson(doc, json);

  for (auto &pair : subscriber_epochs) {
    if (new_epoch > pair.second) {
      pair.first->text(json);
      pair.second = new_epoch;
      Serial.printf("Sent trigger_status to client #%u\n", pair.first->id());
    }
  }
}

// Handles network events (Ethernet/WiFi connect/disconnect)
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
      Serial.println("ETH DNS: " + ETH.dnsIP().toString());
      eth_connected = true;
      network_up = true;
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
      {
        StaticJsonDocument<256> doc;
        doc["type"] = "time_offset";
        doc["offset_minutes"] = config.time_offset_minutes;
        String json;
        serializeJson(doc, json);
        client->text(json);
      }
      subscriber_epochs[client] = 0;
      sendTriggerStatus(last_trigger_info);
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("WebSocket client #%u disconnected\n", client->id());
      subscriber_epochs.erase(client);
      break;
    case WS_EVT_DATA:
      {
        Serial.printf("WebSocket client #%u sent data: %s\n", client->id(), (char *)data);
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, (char *)data, len);
        if (!error) {
          const char *command = doc["command"].as<const char *>();
          if (command && strcmp(command, "subscribe_output_status") == 0) {
            subscriber_epochs[client] = 0;
            Serial.printf("Client #%u subscribed to status\n", client->id());
            sendTriggerStatus(last_trigger_info);
          } else if (command && strcmp(command, "unsubscribe_output_status") == 0) {
            subscriber_epochs.erase(client);
            Serial.printf("Client #%u unsubscribed from status\n", client->id());
          } else if (command && (strcmp(command, "get_output_status") == 0 || strcmp(command, "get_trigger_status") == 0)) {
            sendTriggerStatus(last_trigger_info);
          } else if (command && strcmp(command, "sync_time") == 0) {
            const char *timeStr = doc["time"].as<const char *>();
            if (timeStr) {
              setTimeFromClient(timeStr);
            }
          } else if (command && strcmp(command, "get_network_info") == 0) {
            sendNetworkInfo(client);
          } else if (command && strcmp(command, "save_program") == 0) {
            handleSaveProgram(client, doc);
          } else if (strcmp(command, "get_program") == 0) {
            const char *programID = doc["programID"].as<const char *>();
            if (programID) handleGetProgram(client, programID);
            else sendErrorResponse(client, "", "Missing programID");
          } else if (command && strcmp(command, "set_time_offset") == 0) {
            int offset = 0;
            if (doc["offset_minutes"].is<int>()) {
              offset = doc["offset_minutes"].as<int>();
            }
            if (offset >= -720 && offset <= 840) {
              config.time_offset_minutes = offset;
              saveConfiguration("/config.json", config);
              Serial.printf("Time offset updated to %d minutes\n", offset);
              StaticJsonDocument<256> response;
              response["type"] = "time_offset";
              response["offset_minutes"] = config.time_offset_minutes;
              String json;
              serializeJson(response, json);
              ws.textAll(json);
            } else {
              Serial.println("Invalid time offset received: " + String(offset));
            }
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

// Sends error response to WebSocket client
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

// Finds the next available program ID (01-10)
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

// Sends program save response to WebSocket client
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

// Retrieves a program by ID and sends it to the WebSocket client
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

// Saves a program received via WebSocket
void handleSaveProgram(AsyncWebSocketClient *client, const JsonDocument &doc) {
  const char *programID = doc["programID"].as<const char *>();
  const char *content = doc["content"].as<const char *>();
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
  programsChanged = true;
}

// Sends current time and memory usage to all WebSocket clients
void sendTimeToClients() {
  if (ws.count() > 0) {
    time_t now = time(nullptr);
    unsigned long freeHeap = ESP.getFreeHeap();
    unsigned long usedHeap = totalHeap - freeHeap;

    StaticJsonDocument<512> doc;
    doc["type"] = "time";
    doc["epoch"] = now;
    doc["mem_used"] = usedHeap;
    doc["mem_total"] = totalHeap;
    doc["offset_minutes"] = config.time_offset_minutes;
    doc["millis"] = millis();
    String json;
    serializeJson(doc, json);
    ws.textAll(json);
  }
}

// Sends network information to a WebSocket client
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

// Checks internet connectivity by connecting to a host
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

// Syncs time with NTP servers
bool syncNTPTime() {
  static bool ntpConfigured = false;
  static int retryCount = 0;
  const int maxRetries = 5;
  const char *ntpServers[] = { "216.239.35.0", "pool.ntp.org", "time.google.com", "time.nist.gov" };

  if (!ntpConfigured && network_up) {
    delay(2000);
    for (int i = 0; i < maxRetries; i++) {
      const char *currentServer = ntpServers[retryCount % 4];
      Serial.println("Configuring NTP with server: " + String(currentServer));
      if (currentServer && strlen(currentServer) > 0) {
        configTime(0, 0, currentServer);
        WiFiUDP udp;
        if (!udp.begin(123)) {
          Serial.println("Failed to bind UDP port 123");
          retryCount++;
          delay(2000);
          continue;
        }
        Serial.println("Attempting to reach NTP server: " + String(currentServer));
        IPAddress serverIP;
        if (WiFi.hostByName(currentServer, serverIP)) {
          Serial.println("DNS resolved: " + String(currentServer) + " to " + serverIP.toString());
        } else if (!serverIP.fromString(currentServer)) {
          Serial.println("DNS resolution failed for: " + String(currentServer));
          retryCount++;
          udp.stop();
          delay(2000);
          continue;
        }
        if (udp.beginPacket(serverIP, 123)) {
          Serial.println("NTP server reachable: " + String(currentServer));
          udp.endPacket();
          ntpConfigured = true;
          retryCount = 0;
          udp.stop();
          return true;
        } else {
          Serial.println("Failed to reach NTP server: " + String(currentServer));
          retryCount++;
        }
        udp.stop();
        delay(2000);
      } else {
        Serial.println("NTP server invalid, skipping");
        return false;
      }
    }
    Serial.println("All NTP retries failed");
  }
  return ntpConfigured;
}

// Sets system time from a WebSocket client
void setTimeFromClient(const char *timeStr) {
  if (timeStr) {
    struct tm tm;
    if (sscanf(timeStr, "%d-%d-%d %d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec)
        == 6) {
      tm.tm_year -= 1900;
      tm.tm_mon -= 1;
      tm.tm_isdst = -1;
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

// Checks if internet connectivity should be retested
int shouldCheckInternet() {
  if (network_up && !internet_connected) {
    if (millis() - lastInternetCheck >= internetCheckInterval) {
      Serial.println("Retry internet connectivity check triggered (exponential backoff)");
      return 1;
    }
  }
  return 0;
}

// Writes a file to LittleFS
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

// Reads configuration from LittleFS
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
    config.time_offset_minutes = 0;
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
  strlcpy(config.ssid, doc["ssid"].as<const char *>() ?: "defaultSSID", sizeof(config.ssid));
  strlcpy(config.password, doc["password"].as<const char *>() ?: "defaultPass", sizeof(config.password));
  strlcpy(config.mdns_hostname, doc["mdns_hostname"].as<const char *>() ?: "gday", sizeof(config.mdns_hostname));
  strlcpy(config.ntp_server, doc["ntp_server"].as<const char *>() ?: "pool.ntp.org", sizeof(config.ntp_server));
  strlcpy(config.internet_check_host, doc["internet_check_host"].as<const char *>() ?: "1.1.1.1", sizeof(config.internet_check_host));
  config.time_offset_minutes = doc["time_offset_minutes"].as<int>();
  file.close();
}

// Saves configuration to LittleFS
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
  doc["time_offset_minutes"] = config.time_offset_minutes;
  if (serializeJson(doc, file) == 0) {
    Serial.println(F("Failed to write to file"));
  } else {
    Serial.println(F("Configuration saved"));
  }
  file.close();
}

// Connects to WiFi
void WIFI_Connect() {
  Serial.print("\nConnecting to ");
  Serial.println(config.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.begin(config.ssid, config.password);
  unsigned long startAttemptTime = millis();
  const unsigned long timeout = 10000;
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nFailed to connect to WiFi");
  } else {
    Serial.println("\nWiFi connected");
  }
}

// Parses ISO 8601 date-time (e.g., "2025-04-27T06:00")
time_t parseISO8601(const char *dateTime) {
  struct tm tm = { 0 };
  int year, month, day, hour, minute;
  if (sscanf(dateTime, "%d-%d-%dT%d:%d", &year, &month, &day, &hour, &minute) != 5) {
    return 0;
  }
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = 0;
  tm.tm_isdst = -1;
  return mktime(&tm);
}

// Gets current time adjusted by offset
time_t getAdjustedTime() {
  time_t now = time(nullptr);
  return now + (config.time_offset_minutes * 60);
}

// Checks if a program is active based on date, time, and day
bool isProgramActive(const JsonDocument &doc, int progNum) {
  bool enabled = doc["enabled"].as<bool>();
  if (!enabled) return false;

  time_t now = getAdjustedTime();
  struct tm *timeinfo = localtime(&now);

  bool startDateEnabled = doc["startDateEnabled"].as<bool>();
  const char *startDate = doc["startDate"].as<const char *>();
  if (startDateEnabled && startDate && strlen(startDate) > 0) {
    time_t start = parseISO8601(startDate);
    if (now < start) return false;
  }

  bool endDateEnabled = doc["endDateEnabled"].as<bool>();
  const char *endDate = doc["endDate"].as<const char *>();
  if (endDateEnabled && endDate && strlen(endDate) > 0) {
    time_t end = parseISO8601(endDate);
    if (now > end) return false;
  }

  bool startTimeEnabled = doc["startTimeEnabled"].as<bool>();
  bool endTimeEnabled = doc["endTimeEnabled"].as<bool>();
  const char *startTime = doc["startTime"].as<const char *>();
  const char *endTime = doc["endTime"].as<const char *>();
  if (startTimeEnabled && endTimeEnabled && startTime && endTime && strlen(startTime) > 0 && strlen(endTime) > 0) {
    int startHour, startMinute, endHour, endMinute;
    sscanf(startTime, "%d:%d", &startHour, &startMinute);
    sscanf(endTime, "%d:%d", &endHour, &endMinute);
    int nowMinutes = timeinfo->tm_hour * 60 + timeinfo->tm_min;
    int startMinutes = startHour * 60 + startMinute;
    int endMinutes = endHour * 60 + endMinute;
    if (nowMinutes < startMinutes || nowMinutes > endMinutes) return false;
  }

  bool daysPerWeekEnabled = doc["daysPerWeekEnabled"].as<bool>();
  if (daysPerWeekEnabled) {
    JsonArrayConst selectedDays = doc["selectedDays"];
    if (!selectedDays.isNull() && selectedDays.size() > 0) {
      const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
      const char *currentDay = days[timeinfo->tm_wday];
      bool dayMatch = false;
      for (const char *day : selectedDays) {
        if (strcmp(day, currentDay) == 0) {
          dayMatch = true;
          break;
        }
      }
      if (!dayMatch) return false;
    }
  }

  return true;
}

// Implements Cycle Timer trigger with startHigh option
std::pair<bool, time_t> runCycleTimer(const char *output, CycleState &state, const CycleConfig &config) {
  if (!config.valid || config.runSeconds == 0 || config.stopSeconds == 0) {
    return { false, 0 };
  }

  time_t next_toggle = 0;
  unsigned long now = millis();
  time_t epoch_now = time(nullptr);
  //time_t epoch_now = getAdjustedTime();

  if (!state.isRunning) {
    state.isRunning = true;
    state.lastSwitchTime = now;
    state.isOnPhase = config.startHigh;
    state.nextToggle = epoch_now + (config.startHigh ? config.runSeconds : config.stopSeconds);
    return { state.isOnPhase, state.nextToggle };
  }

  unsigned long elapsed = now - state.lastSwitchTime;
  unsigned long cycle_duration_ms = (state.isOnPhase ? config.runSeconds : config.stopSeconds) * 1000;

  if (elapsed >= cycle_duration_ms) {
    state.isOnPhase = !state.isOnPhase;
    state.lastSwitchTime = now;
    state.nextToggle = epoch_now + (state.isOnPhase ? config.runSeconds : config.stopSeconds);
    return { state.isOnPhase, state.nextToggle };
  }

  // Calculate next toggle time based on the last switch time
  unsigned long time_since_last_switch_ms = now - state.lastSwitchTime;
  unsigned long remaining_ms = cycle_duration_ms - time_since_last_switch_ms;
  // Use ceiling to avoid truncation issues
  next_toggle = epoch_now + (remaining_ms + 999) / 1000;  // Round up to next second
  return { state.isOnPhase, state.nextToggle };
}

void updateOutputs() {
  time_t now = getAdjustedTime();
  if (now < 946684800) {
    digitalWrite(OUTPUT_A_PIN, LOW);
    digitalWrite(OUTPUT_B_PIN, LOW);
    outputAState = outputBState = false;
    return;
  }

  if (programsChanged) {
    activeProgramA = activeProgramB = -1;
    null_program_ids.clear();
    cycleConfigA = cycleConfigB = { 0, 0, false, false };
    for (int i = 1; i <= numPrograms; i++) {
      programConfigs[i] = { -1, "A", "Manual", false };  // Reset
      String filename = "/program" + String(i < 10 ? "0" + String(i) : String(i)) + ".json";
      if (!LittleFS.exists(filename)) continue;
      File file = LittleFS.open(filename, FILE_READ);
      if (!file) continue;
      String content = file.readString();
      file.close();
      StaticJsonDocument<4096> doc;
      if (deserializeJson(doc, content)) continue;
      if (!isProgramActive(doc, i)) continue;
      const char *output = doc["output"].as<const char *>() ?: "A";
      const char *trigger = doc["trigger"].as<const char *>() ?: "Manual";
      programConfigs[i] = { i, String(output), String(trigger), true };
      if (strcmp(output, "A") == 0 && (activeProgramA == -1 || i < activeProgramA)) {
        activeProgramA = i;
        if (strcmp(trigger, "Cycle Timer") == 0) {
          cycleConfigA.runSeconds = (doc["runTime"]["hours"].as<int>() * 3600) + (doc["runTime"]["minutes"].as<int>() * 60) + doc["runTime"]["seconds"].as<int>();
          cycleConfigA.stopSeconds = (doc["stopTime"]["hours"].as<int>() * 3600) + (doc["stopTime"]["minutes"].as<int>() * 60) + doc["stopTime"]["seconds"].as<int>();
          cycleConfigA.startHigh = doc["startHigh"].as<bool>();
          cycleConfigA.valid = true;
        }
      } else if (strcmp(output, "B") == 0 && (activeProgramB == -1 || i < activeProgramB)) {
        activeProgramB = i;
        if (strcmp(trigger, "Cycle Timer") == 0) {
          cycleConfigB.runSeconds = (doc["runTime"]["hours"].as<int>() * 3600) + (doc["runTime"]["minutes"].as<int>() * 60) + doc["runTime"]["seconds"].as<int>();
          cycleConfigB.stopSeconds = (doc["stopTime"]["hours"].as<int>() * 3600) + (doc["stopTime"]["minutes"].as<int>() * 60) + doc["stopTime"]["seconds"].as<int>();
          cycleConfigB.startHigh = doc["startHigh"].as<bool>();
          cycleConfigB.valid = true;
        }
      } else if (strcmp(output, "Null") == 0) {
        null_program_ids.push_back(i);
      }
    }
    programsChanged = false;
  }

  bool trigger_changed = false;
  std::vector<TriggerInfo> trigger_info;
  auto addTriggerInfo = [&](int programId, bool state = false, time_t next_toggle = 0) {
    if (programId == -1 || programId < 1 || programId > numPrograms) return;
    ProgramConfig &cfg = programConfigs[programId];
    if (cfg.id == -1) return;  // Inactive program
    TriggerInfo info = { programId, cfg.output, cfg.trigger, next_toggle, "", state };
    if (cfg.output == "Null" && cfg.trigger != "Manual" && cfg.trigger != "Cycle Timer") {
      info.sensor_value = "0";
    }
    trigger_info.push_back(info);
  };

  // Output A
  if (activeProgramA != -1) {
    bool currentStateA = outputAState;
    if (cycleStateA.activeProgram != activeProgramA) {
      cycleStateA = { false, 0, false, activeProgramA };
      trigger_changed = true;
    }
    if (cycleConfigA.valid) {  // Cycle Timer
      auto [state, next_toggle] = runCycleTimer("Output A", cycleStateA, cycleConfigA);
      outputAState = state;
      addTriggerInfo(activeProgramA, state, next_toggle);
    } else {  // Manual
      outputAState = true;
      addTriggerInfo(activeProgramA, true);
    }
    if (currentStateA != outputAState) {
      trigger_changed = true;
      Serial.printf("Output A changed: Program %d, State %d\n", activeProgramA, outputAState);
    }
  } else {
    if (outputAState) trigger_changed = true;
    outputAState = false;
    cycleStateA = { false, 0, false, -1 };
  }
  digitalWrite(OUTPUT_A_PIN, outputAState ? HIGH : LOW);

  // Output B
  if (activeProgramB != -1) {
    bool currentStateB = outputBState;
    if (cycleStateB.activeProgram != activeProgramB) {
      cycleStateB = { false, 0, false, activeProgramB };
      trigger_changed = true;
    }
    if (cycleConfigB.valid) {  // Cycle Timer
      auto [state, next_toggle] = runCycleTimer("Output B", cycleStateB, cycleConfigB);
      outputBState = state;
      addTriggerInfo(activeProgramB, state, next_toggle);
    } else {  // Manual
      outputBState = true;
      addTriggerInfo(activeProgramB, true);
    }
    if (currentStateB != outputBState) {
      trigger_changed = true;
      Serial.printf("Output B changed: Program %d, State %d\n", activeProgramB, outputBState);
    }
  } else {
    if (outputBState) trigger_changed = true;
    outputBState = false;
    cycleStateB = { false, 0, false, -1 };
  }
  digitalWrite(OUTPUT_B_PIN, outputBState ? HIGH : LOW);

  if (trigger_changed || trigger_info != last_trigger_info) {
    sendTriggerStatus(trigger_info);
    last_trigger_info = trigger_info;
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
  // Initialize output pins
  pinMode(OUTPUT_A_PIN, OUTPUT);
  pinMode(OUTPUT_B_PIN, OUTPUT);
  digitalWrite(OUTPUT_A_PIN, LOW);
  digitalWrite(OUTPUT_B_PIN, LOW);
  delay(100);
  if (!ETH.begin(ETH_PHY_LAN8720, 0, 23, 18, 5, ETH_CLOCK_GPIO17_OUT)) {
    Serial.println("Ethernet failed to start");
  }
  WIFI_Connect();
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  server.onNotFound([](AsyncWebServerRequest *request) {
    if (request->url().startsWith("/assets/")) {
      request->send(404, "text/plain", "Asset not found");
      return;
    }
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
  ElegantOTA.begin(&server, OTA_USERNAME, OTA_PASSWORD);
  server.begin();
}

bool isTimeSynced() {
  time_t now = time(nullptr);
  return now > 946684800;
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
      syncNTPTime();
    }
    lastNTPSyncCheck = millis();
  }
  if (millis() - print_time_count >= 10000) {
    time_t now = time(nullptr);
    char timeStr[50];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", gmtime(&now));
    Serial.print("UTC Time: ");
    Serial.print(timeStr);
    if (wifi_connected) {
      Serial.print(" WiFi IP address: ");
      Serial.print(WiFi.localIP().toString());
    } else if (eth_connected) {
      Serial.print(" Ethernet IP: " + ETH.localIP().toString());
    }
    Serial.println(" Free heap: " + String(ESP.getFreeHeap()));
    print_time_count = millis();
  }
  if (shouldCheckInternet() == 1) {
    checkInternetConnectivity();
  }
  static unsigned long lastTimeSend = 0;
  if (millis() - lastTimeSend > 999) {
    sendTimeToClients();
    lastTimeSend = millis();
  }
  static unsigned long lastOutputUpdate = 0;
  if (millis() - lastOutputUpdate > 100) {
    updateOutputs();
    lastOutputUpdate = millis();
  }
  ElegantOTA.loop();
}