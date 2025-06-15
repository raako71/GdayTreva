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

// Enhanced Sensor structure
struct SensorInfo {
  String type;                         // Sensor type (e.g., "ACS71020", "BME280")
  uint8_t address;                     // I2C address
  bool active;                         // Whether sensor is actively polled
  unsigned long maxPollingInterval;    // Polling interval in ms
  unsigned long lastPollTime;          // Last poll time in ms
  String lastValue;                    // JSON string of latest readings
  bool loggingEnabled;                 // Whether logging is enabled
  bool (*pollFunction)(SensorInfo &);  // Sensor-specific polling function
  std::vector<String> capabilities;    // Available measurements (e.g., ["Temperature", "Humidity", "Pressure"])
  bool operator==(const SensorInfo &other) const {
    return type == other.type && address == other.address && active == other.active && maxPollingInterval == other.maxPollingInterval && lastPollTime == other.lastPollTime && lastValue == other.lastValue && loggingEnabled == other.loggingEnabled && capabilities == other.capabilities;
  }
};

// Sensor polling structure for priority queue
struct SensorPoll {
  SensorInfo *sensor;          // Pointer to sensor info
  unsigned long nextPollTime;  // Next scheduled poll time
  bool operator<(const SensorPoll &other) const {
    return nextPollTime > other.nextPollTime;  // Min-heap (earliest first)
  }
};

// Global variables
std::vector<SensorInfo> detectedSensors;
std::priority_queue<SensorPoll> sensorQueue;
std::vector<String> logBuffer;
unsigned long lastLogWrite = 0;

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

// Enhanced ProgramConfig
struct ProgramConfig {
  int id = -1;                // Program ID (1-10, -1 if invalid)
  String output = "A";        // Output ("A", "B", or "Null")
  String trigger = "Manual";  // Trigger ("Manual", "Cycle Timer", etc.)
  bool active = false;        // Whether the program is active
  String sensorType;          // Associated sensor type
  uint8_t sensorAddress;      // Associated sensor address
};
ProgramConfig programConfigs[11];

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
// I2C Scanner function
void scanI2CSensors() {
  std::vector<SensorInfo> newSensors;
  Serial.println("Scanning I2C bus...");

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
      bool (*pollFunc)(SensorInfo &) = pollGeneric;
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
              pollFunc = pollGeneric;
              capabilities = { "Temperature" };
            } else if ((deviceId >> 8) == 0x41) {  // MCP9600 Device ID
              sensorType = "MCP9601";
              isSensor = true;
              pollingInterval = 1000;
              pollFunc = pollGeneric;
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
              pollFunc = pollACS71020;
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
            pollFunc = pollBME280;
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
            pollFunc = pollVEML6030;
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
            pollFunc = pollSHT3x;
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
            pollFunc = pollGeneric;
            capabilities = { "Temperature", "Humidity" };
            break;
          }
        }
      }

      if (isSensor) {
        SensorInfo sensor = {
          sensorType,
          address,
          false,
          pollingInterval,
          0,
          "{}",
          false,
          pollFunc,
          capabilities
        };
        newSensors.push_back(sensor);
        Serial.printf("Identified %s at address 0x%02X with capabilities: ", sensorType.c_str(), address);
        for (const auto &cap : capabilities) {
          Serial.print(cap + " ");
        }
        Serial.println();
      } else {
        Serial.printf("Unknown device at address 0x%02X\n", address);
      }
    }
  }

  // Compare newSensors with detectedSensors
  sensorsChanged = (newSensors != detectedSensors);
  detectedSensors = newSensors;

  Serial.printf("I2C scan complete. Found %d sensors.\n", detectedSensors.size());
}

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
        serializeJson(doc, sensor.lastValue);
        sensor.lastPollTime = millis();
        return true;
      }
    }
    delay(10);
  }
  return false;
}

bool pollBME280(SensorInfo &sensor) {
  static bool initialized = false;
  if (!initialized) {
    if (!bme280.begin(sensor.address, &Wire)) {
      Serial.printf("BME280 initialization failed at 0x%02X\n", sensor.address);
      return false;
    }
    initialized = true;
  }

  float temp = bme280.readTemperature();
  float hum = bme280.readHumidity();
  float pres = bme280.readPressure() / 100.0F;

  if (isnan(temp) || isnan(hum) || isnan(pres)) {
    Serial.printf("BME280 read failed at 0x%02X\n", sensor.address);
    return false;
  }

  StaticJsonDocument<256> doc;
  doc["temperature"] = temp;
  doc["humidity"] = hum;
  doc["pressure"] = pres;
  serializeJson(doc, sensor.lastValue);
  sensor.lastPollTime = millis();
  return true;
}

bool pollSHT3x(SensorInfo &sensor) {
  // Initialize SHT3x if not already done
  static bool initialized = false;
  if (!initialized) {
    sht3x.begin(Wire, sensor.address);  // Pass Wire and address
    if (sht3x.softReset() != 0) {
      Serial.printf("SHT3x initialization failed at 0x%02X\n", sensor.address);
      return false;
    }
    initialized = true;
  }

  // Read data
  float temp, hum;
  int16_t error = sht3x.measureSingleShot(REPEATABILITY_HIGH, true, temp, hum);  // High repeatability, no clock stretching
  if (error != 0) {
    Serial.printf("SHT3x read failed at 0x%02X, error: 0x%04X\n", sensor.address, error);
    return false;
  }

  StaticJsonDocument<128> doc;
  doc["temperature"] = temp;
  doc["humidity"] = hum;
  serializeJson(doc, sensor.lastValue);
  sensor.lastPollTime = millis();
  return true;
}

bool pollVEML6030(SensorInfo &sensor) {
  const int maxRetries = 3;
  for (int attempt = 1; attempt <= maxRetries; attempt++) {
    Wire.beginTransmission(sensor.address);
    Wire.write(0x00);  // ALS data register
    if (Wire.endTransmission() == 0) {
      Wire.requestFrom(sensor.address, (uint8_t)2);
      if (Wire.available() == 2) {
        uint16_t lux = Wire.read() | (Wire.read() << 8);
        StaticJsonDocument<128> doc;
        doc["lux"] = lux;
        serializeJson(doc, sensor.lastValue);
        sensor.lastPollTime = millis();
        return true;
      }
    }
    delay(10);
  }
  return false;
}

// Generic fallback for unknown sensors
bool pollGeneric(SensorInfo &sensor) {
  const int maxRetries = 3;
  for (int attempt = 1; attempt <= maxRetries; attempt++) {
    Wire.beginTransmission(sensor.address);
    if (Wire.endTransmission() == 0) {
      Wire.requestFrom(sensor.address, (uint8_t)2);
      if (Wire.available() == 2) {
        uint16_t value = Wire.read() | (Wire.read() << 8);
        StaticJsonDocument<128> doc;
        doc["value"] = value;
        serializeJson(doc, sensor.lastValue);
        sensor.lastPollTime = millis();
        return true;
      }
    }
    delay(10);
  }
  return false;
}

// Updated pollSensor dispatcher
bool pollSensor(SensorInfo &sensor) {
  if (sensor.pollFunction) {
    bool success = sensor.pollFunction(sensor);
    if (!success) {
      Serial.printf("Failed to poll %s at 0x%02X\n", sensor.type.c_str(), sensor.address);
    }
    return success;
  }
  return pollGeneric(sensor);
}

// Logs sensor data to buffer
void logSensorData(const SensorInfo &sensor) {
  time_t now = getAdjustedTime();
  char timeStr[20];
  strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", localtime(&now));

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, sensor.lastValue);
  if (error) {
    Serial.printf("Failed to parse sensor JSON for %s: %s\n", sensor.type.c_str(), error.c_str());
    return;
  }

  for (JsonPair kv : doc.as<JsonObject>()) {
    String logEntry = String(timeStr) + "," + sensor.type + ",0x" + String(sensor.address, HEX) + "," + kv.key().c_str() + "," + String(kv.value().as<float>(), 2);
    logBuffer.push_back(logEntry);
  }

  if (logBuffer.size() >= 100 || (millis() - lastLogWrite >= 10000 && !logBuffer.empty())) {
    File file = LittleFS.open("/sensor_log.csv", FILE_APPEND);
    if (file) {
      for (const auto &entry : logBuffer) {
        file.println(entry);
      }
      file.close();
      logBuffer.clear();
      lastLogWrite = millis();
    } else {
      Serial.println("Failed to open /sensor_log.csv for appending");
    }
  }
}
// Polls active sensors
void pollActiveSensors() {
  bool sensorDataChanged = false;
  unsigned long currentTime = millis();
  while (!sensorQueue.empty() && sensorQueue.top().nextPollTime <= currentTime) {
    SensorPoll poll = sensorQueue.top();
    sensorQueue.pop();
    if (poll.sensor->active) {
      String prevValue = poll.sensor->lastValue;
      if (pollSensor(*poll.sensor)) {
        if (poll.sensor->loggingEnabled) {
          logSensorData(*poll.sensor);
        }
        if (poll.sensor->lastValue != prevValue) {
          sensorDataChanged = true;
        }
      }
      SensorPoll nextPoll = { poll.sensor, currentTime + poll.sensor->maxPollingInterval };
      sensorQueue.push(nextPoll);
    }
  }
  if (sensorDataChanged) {
    Serial.println("\nNew Sensor Data.");
    //sendSensorStatus();
  }
}

// Updates active sensors based on programs and logging
void updateActiveSensors() {
  // Clear active flags
  for (auto &sensor : detectedSensors) {
    sensor.active = false;
  }

  // Check programs
  for (int i = 1; i <= numPrograms; i++) {
    if (programConfigs[i].active && !programConfigs[i].sensorType.isEmpty()) {
      for (auto &sensor : detectedSensors) {
        if (sensor.type == programConfigs[i].sensorType && sensor.address == programConfigs[i].sensorAddress) {
          sensor.active = true;
          break;
        }
      }
    }
  }

  // Check logging configuration
  if (LittleFS.exists("/logging.json")) {
    File file = LittleFS.open("/logging.json", FILE_READ);
    if (file) {
      StaticJsonDocument<512> doc;
      if (!deserializeJson(doc, file)) {
        JsonArray sensors = doc.as<JsonArray>();
        for (const auto &entry : sensors) {
          String type = entry["type"].as<String>();
          String addrStr = entry["address"].as<String>();
          if (!type.isEmpty() && !addrStr.isEmpty()) {
            uint8_t address = strtol(addrStr.c_str(), nullptr, 16);
            for (auto &sensor : detectedSensors) {
              if (sensor.type == type && sensor.address == address) {
                sensor.active = true;
                sensor.loggingEnabled = true;
                break;
              }
            }
          }
        }
      }
      file.close();
    }
  }

  // Rebuild polling queue
  while (!sensorQueue.empty()) {
    sensorQueue.pop();
  }
  unsigned long currentTime = millis();
  for (auto &sensor : detectedSensors) {
    if (sensor.active) {
      unsigned long nextPollTime = sensor.lastPollTime + sensor.maxPollingInterval;
      if (nextPollTime < currentTime) {
        nextPollTime = currentTime;  // Immediate poll if overdue
      }
      sensorQueue.push({ &sensor, nextPollTime });
    }
  }
}

// FreeRTOS task for periodic I2C scanningsendSensorStatus
void i2cScanTask(void *parameter) {
  while (true) {
    scanI2CSensors();
    updateActiveSensors();
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
      sensorObj["active"] = sensor.active;
      JsonArray caps = sensorObj.createNestedArray("capabilities");
      for (const auto &cap : sensor.capabilities) {
        caps.add(cap);
      }
      if (sensor.active && !sensor.lastValue.isEmpty()) {
        // Create a temporary JsonDocument to parse lastValue
        StaticJsonDocument<256> tempDoc;
        DeserializationError error = deserializeJson(tempDoc, sensor.lastValue);
        if (!error) {
          sensorObj["value"] = tempDoc.as<JsonObject>();
        } else {
          sensorObj["value"] = nullptr;
          Serial.printf("Failed to parse sensor.lastValue for %s at 0x%02X: %s\n", 
                        sensor.type.c_str(), sensor.address, error.c_str());
        }
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
      Serial.printf("Sent output_status to client #%u\n", pair.first->id());
    }
  }
  last_output_epoch = new_epoch;
}

// Sends trigger status
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
      Serial.printf("Sent trigger_status to client #%u\n", pair.first->id());
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
      sendSensorStatus();
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
  int progId = targetID.toInt();
  String sensorType = contentDoc["sensor"]["type"].as<String>();
  String sensorAddrStr = contentDoc["sensor"]["address"].as<String>();
  uint8_t sensorAddress = sensorAddrStr.isEmpty() ? 0 : strtol(sensorAddrStr.c_str(), nullptr, 16);
  programConfigs[progId].sensorType = sensorType;
  programConfigs[progId].sensorAddress = sensorAddress;
  sendProgramResponse(client, true, "Program saved successfully", targetID.c_str());
  programsChanged = true;
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
    Serial.printf("Invalid NTP server at index %d\n", currentServerIndex);
    attemptCount++;
    currentServerIndex = (currentServerIndex + 1) % numServers;
    return false;
  }

  Serial.printf("Attempting NTP sync with server: %s (Attempt %d/%d)\n",
                currentServer, attemptCount + 1, maxRetries);
  configTime(0, 0, currentServer);
  lastAttemptTime = millis();

  struct tm timeInfo;
  if (getLocalTime(&timeInfo)) {
    Serial.printf("Time synced successfully with %s\n", ntpServers[currentServerIndex]);
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

std::pair<bool, time_t> runCycleTimer(const char *output, CycleState &state, const CycleConfig &config) {
  if (!config.valid || config.runSeconds == 0 || config.stopSeconds == 0) {
    return { false, 0 };
  }

  time_t next_toggle = 0;
  unsigned long now = millis();
  time_t epoch_now = time(nullptr);

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

  unsigned long time_since_last_switch_ms = now - state.lastSwitchTime;
  unsigned long remaining_ms = cycle_duration_ms - time_since_last_switch_ms;
  next_toggle = epoch_now + (remaining_ms + 999) / 1000;
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
      programConfigs[i] = { -1, "A", "Manual", false, "", 0 };
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
      String sensorType = doc["sensor"]["type"].as<String>();
      String sensorAddrStr = doc["sensor"]["address"].as<String>();
      uint8_t sensorAddress = sensorAddrStr.isEmpty() ? 0 : strtol(sensorAddrStr.c_str(), nullptr, 16);
      programConfigs[i] = { i, String(output), String(trigger), true, sensorType, sensorAddress };
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
    updateActiveSensors();
    programsChanged = false;
  }

  bool trigger_changed = false;
  std::vector<TriggerInfo> trigger_info;
  auto addTriggerInfo = [&](int programId, bool state = false, time_t next_toggle = 0) {
    if (programId == -1 || programId < 1 || programId > numPrograms) return;
    ProgramConfig &cfg = programConfigs[programId];
    if (cfg.id == -1) return;
    String sensorValue = "";
    if (cfg.output == "Null" && cfg.trigger != "Manual" && cfg.trigger != "Cycle Timer" && !cfg.sensorType.isEmpty()) {
      for (const auto &sensor : detectedSensors) {
        if (sensor.active && sensor.type == cfg.sensorType && sensor.address == cfg.sensorAddress) {
          sensorValue = sensor.lastValue;
          break;
        }
      }
    }
    trigger_info.push_back({ programId, cfg.output, cfg.trigger, next_toggle, sensorValue, state });
  };

  // Output A
  if (activeProgramA != -1) {
    bool currentStateA = outputAState;
    if (cycleStateA.activeProgram != activeProgramA) {
      cycleStateA = { false, 0, false, activeProgramA };
      trigger_changed = true;
    }
    if (cycleConfigA.valid) {
      auto [state, next_toggle] = runCycleTimer("Output A", cycleStateA, cycleConfigA);
      outputAState = state;
      addTriggerInfo(activeProgramA, state, next_toggle);
    } else {
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
    if (cycleConfigB.valid) {
      auto [state, next_toggle] = runCycleTimer("Output B", cycleStateB, cycleConfigB);
      outputBState = state;
      addTriggerInfo(activeProgramB, state, next_toggle);
    } else {
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

  // Null programs
  for (int id : null_program_ids) {
    addTriggerInfo(id);
  }

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

void loop() {
  static unsigned long lastWiFiRetry = 0;
  static unsigned long lastNTPSyncCheck = 0;
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
    pollActiveSensors();
    updateOutputs();
    lastOutputUpdate = millis();
    if (millis() - lastTimeSend > 999) {
      sendTimeToClients();
      lastTimeSend = millis();
    }
  }
  ElegantOTA.loop();
}