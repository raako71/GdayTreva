#include <SensirionI2cSht3x.h>
#include <Adafruit_BME280.h>
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
#include <map>
#include <vector>
#include <Wire.h>
#include <queue>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define DEBUG_PRINT(level, ...) \
  do { \
    if (debugLevel >= level) Serial.printf(__VA_ARGS__); \
  } while (0)
#define DEBUG_PRINTLN(level, ...) \
  do { \
    if (debugLevel >= level) { \
      Serial.printf(__VA_ARGS__); \
      Serial.println(); \
    } \
  } while (0)

SemaphoreHandle_t littlefsMutex = NULL;

#define FORMAT_LITTLEFS_IF_FAILED true

// Pin definitions for outputs A and B
#define OUTPUT_A_PIN 4
#define OUTPUT_B_PIN 12

// I2C Pin definitions
#define I2C_SDA 32
#define I2C_SCL 33

// OTA Credentials
const char *OTA_USERNAME = "admin";
const char *OTA_PASSWORD = "admin";

// OTA Progress Tracking
static unsigned long otaProgressMillis = 0;

Adafruit_BME280 bme280;
SensirionI2cSht3x sht3x;

// Global variables

std::vector<String> logBuffer;
unsigned long lastLogWrite = 0;
time_t nextTransA, nextTransB;
int activeProgramA = -1, activeProgramB = -1;

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


std::vector<int> null_program_ids;

// Cycle Timer state
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

int debugLevel = 1;

std::map<AsyncWebSocketClient *, uint32_t> subscriber_epochs;
uint32_t last_output_epoch = 0;
std::vector<int> last_null_program_ids;

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

bool sensorsChanged = false;

struct PriorityResult {
  bool isActive;
  time_t nextTransition;
};

// Program scheduling structs
struct ProgramDetails {
  int id;
  bool enabled;
  String output;
  String startDate, endDate, startTime, endTime;
  bool startDateEnabled, endDateEnabled, startTimeEnabled, endTimeEnabled, daysPerWeekEnabled;
  std::vector<String> selectedDays;
  String trigger;
  String sensorType;
  uint8_t sensorAddress;
  String sensorCapability;
  CycleConfig cycleConfig;
};

std::vector<ProgramDetails> ProgramCache;

// Enhanced Sensor structure
struct SensorInfo {
  String type;                       // Sensor type (e.g., "ACS71020", "BME280")
  uint8_t address;                   // I2C address
  unsigned long maxPollingInterval;  // Polling interval in ms
  //bool (*pollFunction)(SensorInfo &);  // Sensor-specific polling function
  std::vector<String> capabilities;  // Available measurements (e.g., ["Temperature", "Humidity", "Pressure"])
  // Define equality operator
  bool operator==(const SensorInfo &other) const {
    return type == other.type && address == other.address && maxPollingInterval == other.maxPollingInterval && capabilities == other.capabilities;
  }
};

std::vector<SensorInfo> detectedSensors;

// I2C Scanner function
void scanI2CSensors() {
  std::vector<SensorInfo> newSensors;
  DEBUG_PRINT(1, "Scanning I2C bus...\n");

  const uint8_t ACS71020_ADDRESSES[] = { 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67 };
  const uint8_t BME280_ADDRESSES[] = { 0x76, 0x77 };
  const uint8_t VEML6030_ADDRESSES[] = { 0x10, 0x48 };
  const uint8_t SHT3X_ADDRESSES[] = { 0x44, 0x45 };
  const uint8_t HDC2080_ADDRESSES[] = { 0x40, 0x41 };
  const uint8_t MCP9600_ADDRESSES[] = { 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67 };

  for (uint8_t address = 0x08; address <= 0x77; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      String sensorType = "Unknown";
      bool isSensor = false;
      unsigned long pollingInterval = 1000;
      //bool (*pollFunc)(SensorInfo &) = pollGeneric;
      std::vector<String> capabilities;

      // Check for sensors in the overlapping address range (0x60 to 0x67)
      if (address >= 0x60 && address <= 0x67) {
        // Attempt to read MCP9600 Device ID register (0x20)
        Wire.beginTransmission(address);
        Wire.write(0x20);  // Device ID register for MCP9600
        if (Wire.endTransmission() == 0) {
          Wire.requestFrom(address, (uint8_t)2);
          if (Wire.available() == 2) {
            uint16_t deviceId = (Wire.read() << 8) | Wire.read();
            if ((deviceId >> 8) == 0x40) {  // MCP9600 Device ID
              sensorType = "MCP9600";
              isSensor = true;
              pollingInterval = 1000;
              //pollFunc = pollGeneric;
              capabilities = { "Temperature" };
            } else if ((deviceId >> 8) == 0x41) {  // MCP9600 Device ID
              sensorType = "MCP9601";
              isSensor = true;
              pollingInterval = 1000;
              //pollFunc = pollGeneric;
              capabilities = { "Temperature" };
            }
          }
        }
        // If not MCP9600, assume ACS71020 for these addresses
        if (!isSensor) {
          for (uint8_t addr : ACS71020_ADDRESSES) {
            if (address == addr) {
              sensorType = "ACS71020";
              isSensor = true;
              pollingInterval = 100;
              //pollFunc = pollACS71020;
              capabilities = { "Current", "Voltage", "Power" };
              break;
            }
          }
        }
      }

      // Check BME280
      if (!isSensor) {
        for (uint8_t addr : BME280_ADDRESSES) {
          if (address == addr) {
            sensorType = "BME280";
            isSensor = true;
            pollingInterval = 1000;
            //pollFunc = pollBME280;
            capabilities = { "Temperature", "Humidity", "Pressure" };
            break;
          }
        }
      }

      // Check VEML6030
      if (!isSensor) {
        for (uint8_t addr : VEML6030_ADDRESSES) {
          if (address == addr) {
            sensorType = "VEML6030";
            isSensor = true;
            pollingInterval = 500;
            //pollFunc = pollVEML6030;
            capabilities = { "Lux" };
            break;
          }
        }
      }

      // Check SHT3x
      if (!isSensor) {
        for (uint8_t addr : SHT3X_ADDRESSES) {
          if (address == addr) {
            sensorType = "SHT3x-DIS";
            isSensor = true;
            pollingInterval = 1000;
            //pollFunc = pollSHT3x;
            capabilities = { "Temperature", "Humidity" };
            break;
          }
        }
      }

      // Check HDC2080
      if (!isSensor) {
        for (uint8_t addr : HDC2080_ADDRESSES) {
          if (address == addr) {
            sensorType = "HDC2080";
            isSensor = true;
            pollingInterval = 1000;
            //pollFunc = pollGeneric;
            capabilities = { "Temperature", "Humidity" };
            break;
          }
        }
      }

      if (isSensor) {
        SensorInfo sensor = {
          sensorType,
          address,
          pollingInterval,
          capabilities
        };
        newSensors.push_back(sensor);
        DEBUG_PRINT(1, "Identified %s at address 0x%02X with capabilities: ", sensorType.c_str(), address);
        for (const auto &cap : capabilities) {
          DEBUG_PRINT(1, "%s ", cap.c_str());  // Fixed line
        }
        DEBUG_PRINT(1, "\n");
      } else {
        DEBUG_PRINT(1, "Unknown device at address 0x%02X\n", address);
      }
    }
  }

  // Compare newSensors with detectedSensors
  sensorsChanged = (newSensors != detectedSensors);
  detectedSensors = newSensors;

  DEBUG_PRINT(1, "I2C scan complete. Found %d sensors.\n", detectedSensors.size());
}
/*
bool pollACS71020(SensorInfo &sensor) {
  const int maxRetries = 3;
  for (int attempt = 1; attempt <= maxRetries; attempt++) {
    Wire.beginTransmission(sensor.address);
    Wire.write(0x20);  // Current register
    if (Wire.endTransmission() == 0) {
      Wire.requestFrom(sensor.address, (uint8_t)2);
      if (Wire.available() == 2) {
        uint16_t raw = (Wire.read() << 8) | Wire.read();
        float current = raw * 0.1;  // Simplified conversion
        StaticJsonDocument<128> doc;
        doc["current"] = current;
        //serializeJson(doc, sensor.lastValue);
        //sensor.lastPollTime = millis();
        return true;
      }
    }
    delay(10);
  }
  return false;
}
*/
/*
bool pollBME280(SensorInfo &sensor) {
  static bool initialized = false;
  if (!initialized) {
    if (!bme280.begin(sensor.address, &Wire)) {
      DEBUG_PRINT(1, "BME280 initialization failed at 0x%02X\n", sensor.address);
      return false;
    }
    initialized = true;
  }

  float temp = bme280.readTemperature();
  float hum = bme280.readHumidity();
  float pres = bme280.readPressure() / 100.0F;

  if (isnan(temp) || isnan(hum) || isnan(pres)) {
    DEBUG_PRINT(1, "BME280 read failed at 0x%02X\n", sensor.address);
    return false;
  }

  StaticJsonDocument<256> doc;
  doc["temperature"] = temp;
  doc["humidity"] = hum;
  doc["pressure"] = pres;
  //serializeJson(doc, sensor.lastValue);
  //sensor.lastPollTime = millis();
  return true;
}
*/
/*
bool pollSHT3x(SensorInfo &sensor) {
  // Initialize SHT3x if not already done
  static bool initialized = false;
  if (!initialized) {
    sht3x.begin(Wire, sensor.address);  // Pass Wire and address
    if (sht3x.softReset() != 0) {
      DEBUG_PRINT(1, "SHT3x initialization failed at 0x%02X\n", sensor.address);
      return false;
    }
    initialized = true;
  }

  // Read data
  float temp, hum;
  int16_t error = sht3x.measureSingleShot(REPEATABILITY_HIGH, true, temp, hum);  // High repeatability, no clock stretching
  if (error != 0) {
    DEBUG_PRINT(1, "SHT3x read failed at 0x%02X, error: 0x%04X\n", sensor.address, error);
    return false;
  }

  StaticJsonDocument<128> doc;
  doc["temperature"] = temp;
  doc["humidity"] = hum;
  //serializeJson(doc, sensor.lastValue);
  //sensor.lastPollTime = millis();
  return true;
}
*/
// FreeRTOS task for periodic I2C scanningsendSensorStatus
void i2cScanTask(void *parameter) {
  while (true) {
    scanI2CSensors();
    vTaskDelay(60000 / portTICK_PERIOD_MS);  // 60 seconds
  }
}

SemaphoreHandle_t sensorMutex = xSemaphoreCreateMutex();
void sendSensorStatus() {
  if (subscriber_epochs.empty()) return;
  if (xSemaphoreTake(sensorMutex, portTICK_PERIOD_MS * 1000) == pdTRUE) {
    uint32_t new_epoch = millis();
    size_t jsonSize = detectedSensors.size() * 100 + 256;
    DynamicJsonDocument doc(jsonSize);
    doc["type"] = "sensor_status";
    doc["epoch"] = new_epoch;
    JsonArray sensors = doc.createNestedArray("sensors");
    for (const auto &sensor : detectedSensors) {
      JsonObject sensorObj = sensors.createNestedObject();
      sensorObj["type"] = sensor.type;
      char addrStr[5];
      snprintf(addrStr, sizeof(addrStr), "0x%02X", sensor.address);
      sensorObj["address"] = addrStr;
      JsonArray caps = sensorObj.createNestedArray("capabilities");
      for (const auto &cap : sensor.capabilities) {
        caps.add(cap);
      }
    }
    char buffer[1024];
    size_t len = serializeJson(doc, buffer, sizeof(buffer));
    if (len == 0) {
      Serial.println("JSON serialization failed");
      xSemaphoreGive(sensorMutex);
      return;
    }
    for (auto &pair : subscriber_epochs) {
      if (new_epoch > pair.second) {
        pair.first->text(buffer);
        pair.second = new_epoch;
      }
    }
    Serial.println("\nSent sensor data");
    xSemaphoreGive(sensorMutex);
  }
}

// Sends output status
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
      DEBUG_PRINT(1, "Sent output_status to client #%u\n", pair.first->id());
    }
  }
  last_output_epoch = new_epoch;
}

// Sends trigger status
void sendTriggerStatus(const std::vector<TriggerInfo> &trigger_info) {
  Serial.println("Sending Trigger status.");
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
      prog["state"] = info.state;
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
      DEBUG_PRINT(1, "Sent trigger_status to client #%u\n", pair.first->id());
    }
  }
}

// Network event handler
void networkEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH Started");
      ETH.setHostname(config.mdns_hostname);
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH Connected");
      eth_connected = true;
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.println("ETH Got IP");
      Serial.println("Ethernet IP: " + ETH.localIP().toString());
      network_up = true;
      checkInternetConnectivity();
      if (!MDNS.begin(config.mdns_hostname)) {
        Serial.println("Error setting up mDNS for Ethernet");
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
      if (!MDNS.begin(config.mdns_hostname)) {
        Serial.println("Error setting up mDNS for WiFi");
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
      DEBUG_PRINT(1, "WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
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
      sendSensorStatus();
      break;
    case WS_EVT_DISCONNECT:
      DEBUG_PRINT(2, "WebSocket client #%u disconnected\n", client->id());
      subscriber_epochs.erase(client);
      break;
    case WS_EVT_DATA:
      {
        DEBUG_PRINT(1, "WebSocket client #%u sent data: %s\n", client->id(), (char *)data);
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, (char *)data, len);
        if (!error) {
          const char *command = doc["command"].as<const char *>();
          if (command && strcmp(command, "subscribe_output_status") == 0) {
            subscriber_epochs[client] = 0;
            DEBUG_PRINT(1, "Client #%u subscribed to status\n", client->id());
            sendTriggerStatus(last_trigger_info);
          } else if (command && strcmp(command, "unsubscribe_output_status") == 0) {
            subscriber_epochs.erase(client);
            DEBUG_PRINT(1, "Client #%u unsubscribed from status\n", client->id());
          } else if (command && strcmp(command, "get_output_status") == 0 || strcmp(command, "get_trigger_status") == 0) {
            sendTriggerStatus(last_trigger_info);
          } else if (command && strcmp(command, "get_sensor_status") == 0) {
            sendSensorStatus();
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
              DEBUG_PRINT(1, "Time offset updated to %d minutes\n", offset);
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

void sendErrorResponse(AsyncWebSocketClient *client, const char *programID, const char *message) {
  StaticJsonDocument<512> response;
  response["type"] = "get_program_response";
  response["success"] = false;
  response["programID"] = programID;
  response["message"] = message;
  String jsonResponse;
  serializeJson(response, jsonResponse);
  client->text(jsonResponse);
  DEBUG_PRINT(1, "Sent error to client #%u: %s\n", client->id(), message);
}

String getNextProgramID() {
  for (int i = 1; i <= 10; i++) {
    String idStr = String(i);
    if (i < 10) idStr = "0" + idStr;
    String filename = "/program" + idStr + ".json";
    if (xSemaphoreTake(littlefsMutex, portTICK_PERIOD_MS * 1000) == pdTRUE) {
      bool exists = LittleFS.exists(filename);
      xSemaphoreGive(littlefsMutex);
      if (!exists) {
        return idStr;
      }
    } else {
      Serial.println("Failed to acquire LittleFS mutex for getNextProgramID");
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
  DEBUG_PRINT(1, "Sent save_program_response to client #%u: %s\n", client->id(), message);
}

void handleGetProgram(AsyncWebSocketClient *client, const char *programID) {
  String idStr = String(programID);
  if (idStr.length() != 2 || idStr < "01" || idStr > "10") {
    sendErrorResponse(client, programID, "Invalid programID (must be 01-10)");
    return;
  }

  String filename = "/program" + idStr + ".json";
  if (xSemaphoreTake(littlefsMutex, portTICK_PERIOD_MS * 1000) == pdTRUE) {
    if (!LittleFS.exists(filename)) {
      xSemaphoreGive(littlefsMutex);
      sendErrorResponse(client, programID, "Program not found");
      return;
    }

    File file = LittleFS.open(filename, FILE_READ);
    if (!file) {
      xSemaphoreGive(littlefsMutex);
      sendErrorResponse(client, programID, "Failed to open program file");
      return;
    }

    String content = file.readString();
    file.close();

    StaticJsonDocument<4096> contentDoc;
    DeserializationError error = deserializeJson(contentDoc, content);
    if (error) {
      xSemaphoreGive(littlefsMutex);
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
    DEBUG_PRINT(1, "Sent program %s to client #%u\n", programID, client->id());
    xSemaphoreGive(littlefsMutex);
  } else {
    sendErrorResponse(client, programID, "Failed to acquire filesystem lock");
  }
}

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

  if (contentDoc["trigger"].as<String>() == "Cycle Timer") {
    int runSeconds = (contentDoc["runTime"]["hours"].as<int>() * 3600) + (contentDoc["runTime"]["minutes"].as<int>() * 60) + contentDoc["runTime"]["seconds"].as<int>();
    int stopSeconds = (contentDoc["stopTime"]["hours"].as<int>() * 3600) + (contentDoc["stopTime"]["minutes"].as<int>() * 60) + contentDoc["stopTime"]["seconds"].as<int>();
    if (runSeconds <= 0 || stopSeconds <= 0) {
      sendProgramResponse(client, false, "Cycle Timer requires non-zero run and stop times", "");
      return;
    }
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
  if (xSemaphoreTake(littlefsMutex, portTICK_PERIOD_MS * 1000) == pdTRUE) {
    File file = LittleFS.open(filename, FILE_WRITE);
    if (!file) {
      xSemaphoreGive(littlefsMutex);
      sendProgramResponse(client, false, "Failed to open file for writing", targetID.c_str());
      return;
    }

    if (serializeJson(contentDoc, file) == 0) {
      file.close();
      xSemaphoreGive(littlefsMutex);
      sendProgramResponse(client, false, "Failed to write program", targetID.c_str());
      return;
    }

    file.close();
    xSemaphoreGive(littlefsMutex);
    sendProgramResponse(client, true, "Program saved successfully", targetID.c_str());
    time_t now = getAdjustedTime();
    updateProgramCache();
  } else {
    sendProgramResponse(client, false, "Failed to acquire filesystem lock", targetID.c_str());
  }
}

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

void sendNetworkInfo(AsyncWebSocketClient *client) {
  String wifi_ip = wifi_connected ? WiFi.localIP().toString() : "N/A";
  String eth_ip = eth_connected ? ETH.localIP().toString() : "N/A";
  String wifi_mac = wifi_connected ? WiFi.macAddress() : "N/A";
  String eth_mac = eth_connected ? ETH.macAddress() : "N/A";
  String wifi_gateway = wifi_connected ? WiFi.gatewayIP().toString() : "N/A";
  String eth_gateway = eth_connected ? ETH.gatewayIP().toString() : "N/A";
  String wifi_subnet = wifi_connected ? WiFi.subnetMask().toString() : "N/A";
  String eth_subnet = eth_connected ? ETH.subnetMask().toString() : "N/A";
  String wifi_dns_str = wifi_connected ? WiFi.dnsIP().toString() : "N/A";
  String wifi_dns_2 = wifi_connected ? WiFi.dnsIP(1).toString() : "N/A";
  String eth_dns_str = eth_connected ? ETH.dnsIP().toString() : "N/A";
  String eth_dns_2 = eth_connected ? ETH.dnsIP(1).toString() : "N/A";
  int32_t wifi_rssi = wifi_connected ? WiFi.RSSI() : 0;
  String hostname = String(config.mdns_hostname) + ".local";

  StaticJsonDocument<512> doc;
  doc["type"] = "network_info";
  if (wifi_connected) {
    doc["wifi_rssi"] = wifi_rssi;
    doc["wifi_ip"] = wifi_ip;
    doc["wifi_gateway"] = wifi_gateway;
    doc["wifi_mac"] = wifi_mac;
    doc["wifi_subnet"] = wifi_subnet;
    doc["wifi_dns"] = wifi_dns_str;
    doc["wifi_dns_2"] = wifi_dns_2;
  }
  if (eth_connected) {
    doc["eth_ip"] = eth_ip;
    doc["eth_gateway"] = eth_gateway;
    doc["eth_subnet"] = eth_subnet;
    doc["eth_dns"] = eth_dns_str;
    doc["eth_dns_2"] = eth_dns_2;
    doc["eth_mac"] = eth_mac;
  }
  doc["mdns_hostname"] = hostname;
  String json;
  serializeJson(doc, json);
  client->text(json);
  Serial.println("Sent network info: " + json);
}

bool checkInternetConnectivity() {
  const char *host = config.internet_check_host;
  const int port = 53;
  WiFiClient client;
  Serial.println("Checking internet connectivity to " + String(host));
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

bool timeSynced = false;
bool syncNTPTime() {
  static const char *ntpServers[] = { "pool.ntp.org", "time.google.com", "time.nist.gov", "216.239.35.0" };
  static const int numServers = 4;
  static const int maxRetries = 5;
  static const unsigned long retryInterval = 5000;
  static int currentServerIndex = 0;
  static int attemptCount = 0;
  static unsigned long lastAttemptTime = 0;

  if ((millis() - lastAttemptTime < retryInterval)) {
    return false;
  }

  if (attemptCount >= maxRetries) {
    Serial.println("All NTP sync attempts failed");
    attemptCount = 0;
    currentServerIndex = 0;
    return false;
  }

  const char *currentServer = ntpServers[currentServerIndex];
  if (!currentServer || strlen(currentServer) == 0) {
    DEBUG_PRINT(1, "Invalid NTP server at index %d\n", currentServerIndex);
    attemptCount++;
    currentServerIndex = (currentServerIndex + 1) % numServers;
    return false;
  }

  DEBUG_PRINT(1, "Attempting NTP sync with server: %s (Attempt %d/%d)\n",
              currentServer, attemptCount + 1, maxRetries);
  configTime(0, 0, currentServer);
  lastAttemptTime = millis();

  struct tm timeInfo;
  if (getLocalTime(&timeInfo)) {
    DEBUG_PRINT(1, "Time synced successfully with %s\n", ntpServers[currentServerIndex]);
    timeSynced = true;
    attemptCount = 0;
    currentServerIndex = 0;
    return true;
  }
  attemptCount++;
  currentServerIndex = (currentServerIndex + 1) % numServers;
  return false;
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

int shouldCheckInternet() {
  if (network_up && !internet_connected) {
    if (millis() - lastInternetCheck >= internetCheckInterval) {
      Serial.println("Retry internet connectivity check triggered");
      return 1;
    }
  }
  return 0;
}

void writeFile(const char *path, const char *message) {
  DEBUG_PRINT(1, "Writing file: %s\r\n", path);
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
  if (xSemaphoreTake(littlefsMutex, portTICK_PERIOD_MS * 1000) == pdTRUE) {
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
      xSemaphoreGive(littlefsMutex);
      return;
    }
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, file);
    if (error) {
      Serial.print(F("Failed to read file!!!: "));
      Serial.println(error.c_str());
      file.close();
      xSemaphoreGive(littlefsMutex);
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
    xSemaphoreGive(littlefsMutex);
  } else {
    Serial.println("Failed to acquire LittleFS mutex for readSettings");
  }
}

void saveConfiguration(const char *filename, const Config &config) {
  if (xSemaphoreTake(littlefsMutex, portTICK_PERIOD_MS * 1000) == pdTRUE) {
    File file = LittleFS.open(filename, FILE_WRITE);
    if (!file) {
      Serial.println(F("Failed to create file"));
      xSemaphoreGive(littlefsMutex);
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
    xSemaphoreGive(littlefsMutex);
  } else {
    Serial.println("Failed to acquire LittleFS mutex for saveConfiguration");
  }
}

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

time_t getAdjustedTime() {
  time_t now = time(nullptr);
  return now + (config.time_offset_minutes * 60);
}

std::pair<bool, bool> runCycleTimer(const char *output, CycleState &state, const CycleConfig &config) {

  if (!config.valid || config.runSeconds == 0 || config.stopSeconds == 0) {
    DEBUG_PRINT(3, "runCycleTimer: Output=%s, INVALID config (valid=%d, runSeconds=%d, stopSeconds=%d)\n", output, config.valid, config.runSeconds, config.stopSeconds);
    return { false, 0 };
  } else {
    DEBUG_PRINT(2, "runCycleTimer: Output=%s, Valid config (valid=%d, runSeconds=%d, stopSeconds=%d)\n", output, config.valid, config.runSeconds, config.stopSeconds);
  }
  time_t next_toggle = 0;
  unsigned long currentMillis = millis();
  time_t currentEpoch = time(nullptr);
  if (!state.isRunning) {
    state.isRunning = true;
    state.lastSwitchTime = currentMillis;
    state.isOnPhase = config.startHigh;
    state.nextToggle = currentEpoch + (config.startHigh ? config.runSeconds : config.stopSeconds);
    return { state.isOnPhase, state.nextToggle };
  }
  unsigned long elapsedMillis = currentMillis - state.lastSwitchTime;
  unsigned long cycleDurationMillis = (state.isOnPhase ? config.runSeconds : config.stopSeconds) * 1000;
  if (elapsedMillis >= cycleDurationMillis) {
    state.isOnPhase = !state.isOnPhase;
    state.lastSwitchTime = currentMillis;
    state.nextToggle = currentEpoch + (state.isOnPhase ? config.runSeconds : config.stopSeconds);
    return { state.isOnPhase, state.nextToggle };
  }
  unsigned long remainingMillis = cycleDurationMillis - elapsedMillis;
  state.nextToggle = currentEpoch + (remainingMillis + 999) / 1000;
  return { state.isOnPhase, state.nextToggle };
}

//determine active status and next transition for a program.
PriorityResult determineProgramPriority(const ProgramDetails &prog, time_t now) {
  PriorityResult result = { false, -1 };
  if (!prog.enabled) {
    DEBUG_PRINT(3, "Program %d: Disabled\n", prog.id);
    return result;
  }

  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  time_t startEpoch = -1, endEpoch = LONG_MAX;

  bool activeNow = 1;
  // start date
  if (prog.startDateEnabled && !prog.startDate.isEmpty()) {
    startEpoch = parseISO8601(prog.startDate.c_str());
    DEBUG_PRINTLN(3, "Program %d: startEpoch = %ld", prog.id, startEpoch);
    if (now < startEpoch) {
      activeNow = 0;
    }
  }
  // end date
  if (prog.endDateEnabled && !prog.endDate.isEmpty()) {
    endEpoch = parseISO8601(prog.endDate.c_str());
    DEBUG_PRINTLN(3, "Program %d: endEpoch = %ld", prog.id, endEpoch);
    if (now >= endEpoch) {
      DEBUG_PRINT(3, "Program %d: expired\n", prog.id);
      return result;
    }
  }

  // Handle start time
  int startHour = 0, startMinute = 0;
  if (prog.startTimeEnabled && !prog.startTime.isEmpty()) {
    sscanf(prog.startTime.c_str(), "%d:%d", &startHour, &startMinute);
    struct tm startTime = timeinfo;  // Copy current day's timeinfo
    startTime.tm_hour = startHour;
    startTime.tm_min = startMinute;
    startTime.tm_sec = 0;
    time_t startTimeEpoch = mktime(&startTime);
    if (now < startTimeEpoch) {
      activeNow = 0;
      if (startTimeEpoch < startEpoch && startEpoch > 0) startEpoch = startTimeEpoch;
    }
  }
  // Handle end time
  int endHour = 23, endMinute = 59;
  if (prog.endTimeEnabled && !prog.endTime.isEmpty()) {
    sscanf(prog.endTime.c_str(), "%d:%d", &endHour, &endMinute);
    struct tm endTime = timeinfo;  // Copy current day's timeinfo
    endTime.tm_hour = endHour;
    endTime.tm_min = endMinute;
    endTime.tm_sec = 0;
    time_t endTimeEpoch = mktime(&endTime);
    if (now >= endTimeEpoch) {
      DEBUG_PRINT(3, "Program %d: expired\n", prog.id);
      return result;
    } else if (endTimeEpoch < endEpoch && endEpoch > 0) endEpoch = endTimeEpoch;
  }

  // Handle days of the week
  if (prog.daysPerWeekEnabled) {
    if (!prog.selectedDays.empty()) {
      const char *days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
      int currentDay = timeinfo.tm_wday;
      if (std::find(prog.selectedDays.begin(), prog.selectedDays.end(), days[currentDay]) == prog.selectedDays.end()) {
        DEBUG_PRINT(3, "Today inactive for Program %d\n", prog.id);
        // Find the next active day
        int nextDayIndex = -1;
        for (int i = 1; i <= 7; i++) {
          int checkDay = (currentDay + i) % 7;  // Cycle through days of the week
          if (std::find(prog.selectedDays.begin(), prog.selectedDays.end(), days[checkDay]) != prog.selectedDays.end()) {
            nextDayIndex = checkDay;
            break;
          }
        }
        if (nextDayIndex != -1) {
          // Calculate time_t for 00:00 of the next active day
          struct tm nextDayTm = timeinfo;  // Copy current timeinfo
          nextDayTm.tm_hour = 0;
          nextDayTm.tm_min = 0;
          nextDayTm.tm_sec = 0;
          int daysToAdd = (nextDayIndex - currentDay + 7) % 7;  // Days until next active day
          if (daysToAdd == 0) daysToAdd = 7;                    // If same day, schedule for next week
          nextDayTm.tm_mday += daysToAdd;
          mktime(&nextDayTm);               // Normalize the time structure
          startEpoch = mktime(&nextDayTm);  // Convert to epoch time
        }
        result.nextTransition = startEpoch;
        return result;
      }
    } else {
      DEBUG_PRINT(3, "No active days for Program %d\n", prog.id);
      return result;
    }
  }
  if (activeNow) {
    result = { true, endEpoch };
    DEBUG_PRINT(1, "Program %d: Active=%d, Next transition=%ld\n", prog.id, result.isActive, result.nextTransition);
    return result;
  } else {
    result = { false, startEpoch };
    DEBUG_PRINT(1, "Program %d: Active=%d, Next transition=%ld\n", prog.id, result.isActive, result.nextTransition);
    return result;
  }
}

//set active programs and transition times.
void setProgramPriorities(time_t now) {
  activeProgramA = -1;
  activeProgramB = -1;
  nextTransA = LONG_MAX;
  nextTransB = LONG_MAX;

  for (const auto &prog : ProgramCache) {
    if (!prog.enabled && prog.output != "null") {
      continue;
    }

    PriorityResult result = determineProgramPriority(prog, now);

    // Handle active programs (lowest ID for each output)
    if (result.isActive) {
      if (prog.output == "A" && (activeProgramA == -1 || prog.id < activeProgramA)) {
        activeProgramA = prog.id;
        //DEBUG_PRINT(3, "Program %d: Set as activeProgramA\n", prog.id);
      } else if (prog.output == "B" && (activeProgramB == -1 || prog.id < activeProgramB)) {
        activeProgramB = prog.id;
        //DEBUG_PRINT(3, "Program %d: Set as activeProgramB\n", prog.id);
      }
    }

    // Handle next transition (earliest epoch for each output)
    if (result.nextTransition >= 0) {
      if (prog.output == "A" && result.nextTransition < nextTransA) {
        nextTransA = result.nextTransition;
        //DEBUG_PRINT(3, "Program %d: Updated nextTransA to %ld\n", prog.id, nextTransA);
      } else if (prog.output == "B" && result.nextTransition < nextTransB) {
        nextTransB = result.nextTransition;
        //DEBUG_PRINT(3, "Program %d: Updated nextTransB to %ld\n", prog.id, nextTransB);
      }
    }
  }

  DEBUG_PRINT(1, "Programs set: activeProgramA=%d, activeProgramB=%d, nextTransA=%ld, nextTransB=%ld\n",
              activeProgramA, activeProgramB, nextTransA, nextTransB);
}

//update program cache from flash.
void updateProgramCache() {
  DEBUG_PRINTLN(1, "Updating program cache...");
  ProgramCache.clear();
  null_program_ids.clear();

  time_t now = getAdjustedTime();

  for (int i = 1; i <= numPrograms; i++) {
    String filename = "/program" + String(i < 10 ? "0" + String(i) : String(i)) + ".json";
    if (xSemaphoreTake(littlefsMutex, portTICK_PERIOD_MS * 1000) == pdTRUE) {
      if (!LittleFS.exists(filename)) {
        DEBUG_PRINT(2, "Program %d: File %s does not exist\n", i, filename.c_str());
        xSemaphoreGive(littlefsMutex);
        continue;
      }
      File file = LittleFS.open(filename, FILE_READ);
      if (!file) {
        xSemaphoreGive(littlefsMutex);
        DEBUG_PRINT(1, "Program %d: Failed to open file %s\n", i, filename.c_str());
        continue;
      }
      String content = file.readString();
      file.close();
      xSemaphoreGive(littlefsMutex);
      StaticJsonDocument<4096> doc;
      if (deserializeJson(doc, content)) {
        DEBUG_PRINT(1, "Program %d: Failed to parse JSON\n", i);
        continue;
      }

      ProgramDetails details;
      details.id = i;
      details.enabled = doc["enabled"].as<bool>();
      details.output = doc["output"].as<String>() ?: "null";
      details.startDate = doc["startDate"].as<String>();
      details.startDateEnabled = doc["startDateEnabled"].as<bool>();
      details.endDate = doc["endDate"].as<String>();
      details.endDateEnabled = doc["endDateEnabled"].as<bool>();
      details.startTime = doc["startTime"].as<String>();
      details.startTimeEnabled = doc["startTimeEnabled"].as<bool>();
      details.endTime = doc["endTime"].as<String>();
      details.endTimeEnabled = doc["endTimeEnabled"].as<bool>();
      JsonArray daysArray = doc["selectedDays"];
      details.selectedDays.clear();
      for (JsonVariant day : daysArray) {
        details.selectedDays.push_back(day.as<String>());
      }
      details.daysPerWeekEnabled = doc["daysPerWeekEnabled"].as<bool>();
      details.trigger = doc["trigger"].as<String>();
      details.sensorType = doc["sensorType"].as<String>() ?: "";
      details.sensorAddress = strtol(doc["sensorAddress"].as<const char *>() ?: "0", nullptr, 16);
      details.sensorCapability = doc["sensorCapability"].as<String>() ?: "";
      details.cycleConfig = { 0, 0, false, false };  // Default initialization
      if (details.trigger == "Cycle Timer") {
        details.cycleConfig.runSeconds = (doc["runTime"]["hours"].as<int>() * 3600) + (doc["runTime"]["minutes"].as<int>() * 60) + doc["runTime"]["seconds"].as<int>();
        details.cycleConfig.stopSeconds = (doc["stopTime"]["hours"].as<int>() * 3600) + (doc["stopTime"]["minutes"].as<int>() * 60) + doc["stopTime"]["seconds"].as<int>();
        details.cycleConfig.startHigh = doc["startHigh"].as<bool>();
        details.cycleConfig.valid = (details.cycleConfig.runSeconds > 0 && details.cycleConfig.stopSeconds > 0);
        DEBUG_PRINT(3, "Program %d: Parsed CycleConfig - runSeconds=%d, stopSeconds=%d, startHigh=%d, valid=%d\n",
                    i, details.cycleConfig.runSeconds, details.cycleConfig.stopSeconds,
                    details.cycleConfig.startHigh, details.cycleConfig.valid);
      }
      if (details.output.equalsIgnoreCase("null") && details.enabled) {
        null_program_ids.push_back(details.id);
        DEBUG_PRINT(1, "Program %d: Added to null_program_ids\n", i);
      } else {
        DEBUG_PRINT(3, "Program %d: Loaded, Output=%s, Enabled=%d\n", i, details.output.c_str(), details.enabled);
        ProgramCache.push_back(details);
      }
    } else {
      DEBUG_PRINT(1, "Program %d: Failed to acquire LittleFS mutex\n", i);
    }
  }
  DEBUG_PRINTLN(1, "Program cache updated.");
  setProgramPriorities(now);
}

void updateOutputs() {
}

void updateDebugLevel() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');  // Read until newline
    input.trim();                                 // Remove whitespace
    if (input.length() > 0) {
      int newLevel = input.toInt();
      if (newLevel >= 0 && newLevel <= 3) {  // Validate range
        debugLevel = newLevel;
        DEBUG_PRINT(0, "Debug level set to %d\n", debugLevel);
      } else {
        Serial.println("Invalid debug level. Use 0-3.");
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nboot");
  totalHeap = ESP.getHeapSize();
  Serial.println("Total heap size: " + String(totalHeap) + " bytes");

  littlefsMutex = xSemaphoreCreateMutex();
  if (littlefsMutex == NULL) {
    Serial.println("Failed to create LittleFS mutex");
    return;
  }

  // Initialize I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.println("I2C initialized on SDA: " + String(I2C_SDA) + ", SCL: " + String(I2C_SCL));

  // Create FreeRTOS task for I2C scanning
  xTaskCreatePinnedToCore(i2cScanTask, "I2CScan", 4096, NULL, 1, NULL, 0);

  WiFi.onEvent(networkEvent);
  if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED)) {
    Serial.println("LittleFS Mount Failed");
    return;
  }
  readSettings();
  pinMode(5, OUTPUT);
  digitalWrite(5, HIGH);
  pinMode(OUTPUT_A_PIN, OUTPUT);
  pinMode(OUTPUT_B_PIN, OUTPUT);
  digitalWrite(OUTPUT_A_PIN, LOW);
  digitalWrite(OUTPUT_B_PIN, LOW);
  delay(100);
  if (!ETH.begin(ETH_PHY_LAN8720, 0, 23, 18, 5, ETH_CLOCK_GPIO17_OUT)) {
    Serial.println("Ethernet failed to start");
  }
  if (!eth_connected) WIFI_Connect();
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
      if (xSemaphoreTake(littlefsMutex, portTICK_PERIOD_MS * 1000) == pdTRUE) {
        if (LittleFS.exists("/" + filename)) {
          Serial.println("File exists");
          File file = LittleFS.open("/" + filename, "r");
          if (file) {
            Serial.println("File opened, size: " + String(file.size()));
            String content = file.readString();
            Serial.println("File content: " + content);
            Serial.println("Free heap: " + String(ESP.getFreeHeap()));
            file.close();
            xSemaphoreGive(littlefsMutex);
            request->send(200, "text/plain", content);
            return;
          }
          Serial.println("Failed to open file");
          xSemaphoreGive(littlefsMutex);
          request->send(404, "text/plain", "File not found");
        } else {
          Serial.println("File does not exist");
          xSemaphoreGive(littlefsMutex);
          request->send(404, "text/plain", "File not found");
        }
      } else {
        Serial.println("Failed to acquire LittleFS mutex for /getFile");
        request->send(500, "text/plain", "Filesystem lock timeout");
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
  updateProgramCache();
}

void loop() {
  static unsigned long lastWiFiRetry = 0;
  static unsigned long lastNTPSyncCheck = 0;
  updateDebugLevel();
  if (!wifi_connected && !eth_connected && millis() - lastWiFiRetry >= 10000) {
    WIFI_Connect();
    lastWiFiRetry = millis();
  }

  if (!timeSynced && network_up) syncNTPTime();

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
  if (sensorsChanged) {
    sendSensorStatus();
    sensorsChanged = false;
  }
  static unsigned long lastTimeSend = 0;
  static unsigned long lastOutputUpdate = 0;
  if (millis() - lastOutputUpdate > 100) {
    //pollActiveSensors();
    updateOutputs();
    lastOutputUpdate = millis();
    if (millis() - lastTimeSend > 999) {
      sendTimeToClients();
      lastTimeSend = millis();
    }
  }
  ElegantOTA.loop();
}