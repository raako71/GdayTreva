#include <Arduino.h>
#include <vector>
#include <Seeed_MCP9600.h>
#include <SparkFun_VEML6030_Ambient_Light_Sensor.h>
#include <SensirionI2cSht3x.h>
#include <HDC2080.h>
#include <Adafruit_BME280.h>
#include <ETH.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <time.h>
#include <sys/time.h>
//#include <ElegantOTA.h>
#include <map>
#include <Wire.h>
#include <queue>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct SensorInfo;
struct Config;

SemaphoreHandle_t littlefsMutex = NULL;
SemaphoreHandle_t i2cMutex = NULL;

#define DEBUG_PRINT(level, ...) \
  do { \
    if (debugLevel >= level && Serial) Serial.printf(__VA_ARGS__); \
  } while (0)
#define DEBUG_PRINTLN(level, ...) \
  do { \
    if (debugLevel >= level && Serial) { \
      Serial.printf(__VA_ARGS__); \
      Serial.println(); \
    } \
  } while (0)

int debugLevel = 0;

#define FORMAT_LITTLEFS_IF_FAILED true

// Hardware pin definitions
#define OUTPUT_A_PIN 4
#define OUTPUT_B_PIN 12
#define I2C_SDA 32
#define I2C_SCL 33
#define WIFI_BTN 13
#define WIFI_LED 14
#define INPUT_A 35
#define INPUT_B 39
#define IO2 2    //RED LED
#define IO34 34  //Remote Button
#define IO36 36  //Unused IO
#define IO15 15  //LEDs Enable

// OTA Credentials
const char *OTA_USERNAME = "admin";
const char *OTA_PASSWORD = "admin";

// OTA Progress Tracking
static unsigned long otaProgressMillis = 0;

struct ACSReading {
  float current;  // Amperes, 2 decimal places
  int voltage;    // Volts, integer
  int power;      // Watts, integer
};

// Global variables
std::vector<String> logBuffer;
unsigned long lastLogWrite = 0;
time_t nextTransA, nextTransB;
int activeProgramA = -1, activeProgramB = -1;
std::map<String, String> sensorReadingsCache;
std::map<String, String> prevCache;
unsigned long cacheUpdated = 0;
std::vector<ACSReading> acsReadingsBuffer;
bool acsValid = true;
unsigned long lastACSPoll = 0;
const unsigned long cacheUpdateInterval = 1000;
const unsigned long acsPollInterval = 100;

time_t NextCycleToggleA = 0, NextCycleToggleB = 0;

// Helper functions to lock/unlock I2C mutex
void i2cLock() {
  xSemaphoreTake(i2cMutex, portTICK_PERIOD_MS * 1000);
}
void i2cUnlock() {
  xSemaphoreGive(i2cMutex);
}

struct Config {
  char ssid[32];
  char password[64];
  char mdns_hostname[32];
  char ntp_server[32];
  char internet_check_host[32];
  int time_offset_minutes;
  char device_name[26];
};
Config config;

struct CycleConfig {
  unsigned int runSeconds = 0;
  unsigned int stopSeconds = 0;
  bool startHigh = false;
  bool valid = false;
};

std::vector<int> null_program_ids;

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

bool sensorsChanged = false;

struct PriorityResult {
  bool isActive;
  time_t nextTransition;
};


struct ProgramDetails {
  String name;
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

bool runCycleTimer(const char *output, bool active, const ProgramDetails &prog);
PriorityResult determineProgramPriority(const ProgramDetails &prog, time_t now);
void saveConfiguration(const char *filename, const Config &config);

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

  i2cLock();
  for (uint8_t address = 0x08; address <= 0x77; address++) {
    Wire.beginTransmission(address);
    if (Wire.endTransmission() == 0) {
      String sensorType = "Unknown";
      bool isSensor = false;
      unsigned long pollingInterval = 1000;
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
              capabilities = { "Temperature" };
            } else if ((deviceId >> 8) == 0x41) {  // MCP9600 Device ID
              sensorType = "MCP9601";
              isSensor = true;
              pollingInterval = 1000;
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
          DEBUG_PRINT(1, "%s ", cap.c_str());
        }
        DEBUG_PRINT(1, "\n");
      } else {
        DEBUG_PRINT(1, "Unknown device at address 0x%02X\n", address);
      }
    }
  }
  i2cUnlock();

  // Compare newSensors with detectedSensors
  sensorsChanged = (newSensors != detectedSensors);
  detectedSensors = newSensors;

  DEBUG_PRINT(1, "I2C scan complete. Found %d sensors.\n", detectedSensors.size());
}

// FreeRTOS task for periodic I2C scanning
void i2cScanTask(void *parameter) {
  while (true) {
    scanI2CSensors();
    vTaskDelay(60000 / portTICK_PERIOD_MS);  // 60 seconds
  }
}

static unsigned long lastSensorRequest = 0;
void requestSensorScan() {
  if (millis() - lastSensorRequest > 5000) {
    scanI2CSensors();
    lastSensorRequest = millis();
  }
}

void sendDiscoveredSensors() {
  if (subscriber_epochs.empty()) return;
    uint32_t new_epoch = millis();
    size_t jsonSize = detectedSensors.size() * 128 + 256;  // Increase estimate
    DynamicJsonDocument doc(jsonSize);
    doc["type"] = "discovered_sensors";
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
    size_t requiredSize = measureJson(doc);
    if (requiredSize > 2048) {  // Increased buffer size
      Serial.println("DiscoveredSensors JSON too large: " + String(requiredSize) + " bytes");
      return;
    }
    char buffer[2048];
    size_t len = serializeJson(doc, buffer, sizeof(buffer));
    if (len == 0) {
      Serial.println("JSON serialization failed");
      return;
    }
    DEBUG_PRINT(1, "Sending discovered_sensors JSON (%d bytes): %s\n", len, buffer);
    for (auto &pair : subscriber_epochs) {
      if (new_epoch > pair.second) {
        pair.first->text(buffer);
        pair.second = new_epoch;
      }
    }
    DEBUG_PRINTLN(1, "Sensor data sent.");
}

void sendActiveprograms() {
  if (subscriber_epochs.empty()) return;
  DEBUG_PRINTLN(3, "Sending Active Program Data.");
  uint32_t new_epoch = millis();
  StaticJsonDocument<512> doc;
  doc["type"] = "active_program_data";
  doc["epoch"] = new_epoch;

  JsonArray sensors = doc.createNestedArray("sensors");
  for (const auto &pair : sensorReadingsCache) {
    JsonObject sensor = sensors.createNestedObject();
    // Parse key (e.g., "ACS71020:0x62:Power") into type, address, capability
    String key = pair.first;
    int colon1 = key.indexOf(':');
    int colon2 = key.lastIndexOf(':');
    if (colon1 != -1 && colon2 != -1 && colon1 != colon2) {
      String type = key.substring(0, colon1);
      String address = key.substring(colon1 + 1, colon2);
      String capability = key.substring(colon2 + 1);
      sensor["type"] = type;
      sensor["address"] = address;
      sensor["capability"] = capability;
      sensor["value"] = pair.second;
    } else {
      // Fallback if key format is invalid
      sensor["key"] = key;
      sensor["value"] = pair.second;
    }
  }

  String json;
  serializeJson(doc, json);
  DEBUG_PRINT(3, "JSON to send: %s\n", json.c_str());

  for (auto &pair : subscriber_epochs) {
    if (new_epoch > pair.second) {
      pair.first->text(json);
      pair.second = new_epoch;
      DEBUG_PRINT(2, "Sent trigger_status to client #%u\n", pair.first->id());
    }
  }
}
std::vector<SensorInfo> activeSensors;

// Helper function to parse hex address (e.g., "0x62" to uint8_t 0x62)
uint8_t parseHexAddress(const String &hexStr) {
  String cleanHex = hexStr;
  cleanHex.replace("0x", "");
  return (uint8_t)strtol(cleanHex.c_str(), nullptr, 16);
}

// Function to add sensor to activeSensors if unique
bool addSensorIfUnique(const String &sensorType, uint8_t sensorAddress, const String &capability, std::vector<SensorInfo> &activeSensors) {
  // Check if sensor already exists
  auto it = std::find_if(activeSensors.begin(), activeSensors.end(),
                         [&](const SensorInfo &s) {
                           return s.type == sensorType && s.address == sensorAddress;
                         });
  if (it != activeSensors.end()) {
    // Sensor exists, add capability if not already present
    if (std::find(it->capabilities.begin(), it->capabilities.end(), capability) == it->capabilities.end()) {
      it->capabilities.push_back(capability);
      DEBUG_PRINT(1, "Added capability %s to existing sensor: %s at 0x%02X\n",
                  capability.c_str(), sensorType.c_str(), sensorAddress);
    }
    return false;
  }
  // New sensor, set polling interval based on type
  unsigned long pollingInterval = 1000;  // Default
  if (sensorType == "VEML6030") pollingInterval = 500;
  else if (sensorType == "ACS71020") pollingInterval = 100;
  SensorInfo sensor = { sensorType, sensorAddress, pollingInterval, { capability } };
  activeSensors.push_back(sensor);
  DEBUG_PRINT(1, "Added active sensor: %s at 0x%02X with capability: %s\n",
              sensorType.c_str(), sensorAddress, capability.c_str());
  return true;
}

void updateActiveSensors() {
  DEBUG_PRINT(1, "updateActiveSensors\n");
  activeSensors.clear();
  // Process null_program_ids
  for (const int id : null_program_ids) {
    DEBUG_PRINT(1, "NULL Prog ID: %d\n", id);
    auto it = std::find_if(ProgramCache.begin(), ProgramCache.end(),
                           [&](const ProgramDetails &p) {
                             return p.id == id;
                           });
    if (it == ProgramCache.end()) {
      DEBUG_PRINT(2, "Program %d: Not found in ProgramCache\n", id);
      continue;
    }
    if (!it->enabled) {
      DEBUG_PRINT(1, "Program %d: Disabled\n", id);
    }
    if (it->trigger != "Sensor") {
      DEBUG_PRINT(1, "Program %d: Trigger is '%s', expected 'Sensor'\n", id, it->trigger.c_str());
    }
    if (it->sensorType.isEmpty()) {
      DEBUG_PRINT(1, "Program %d: sensorType is empty\n", id);
    }
    if (it->sensorAddress == 0) {
      DEBUG_PRINT(1, "Program %d: sensorAddress is 0 (invalid)\n", id);
    }
    if (it->sensorCapability.isEmpty()) {
      DEBUG_PRINT(1, "Program %d: sensorCapability is empty\n", id);
    }
    if (it->enabled && it->trigger == "Sensor" && !it->sensorType.isEmpty() && it->sensorAddress != 0 && !it->sensorCapability.isEmpty()) {
      DEBUG_PRINT(1, "Adding sensor from null_program_id %d: Type=%s, Address=0x%02X, Capability=%s\n",
                  id, it->sensorType.c_str(), it->sensorAddress, it->sensorCapability.c_str());
      addSensorIfUnique(it->sensorType, it->sensorAddress, it->sensorCapability, activeSensors);
    }
  }

  // Process activeProgramA
  if (activeProgramA != -1) {
    auto it = std::find_if(ProgramCache.begin(), ProgramCache.end(),
                           [&](const ProgramDetails &p) {
                             return p.id == activeProgramA;
                           });
    if (it != ProgramCache.end() && it->enabled && it->trigger == "Sensor" && !it->sensorType.isEmpty() && it->sensorAddress != 0 && !it->sensorCapability.isEmpty()) {
      DEBUG_PRINT(1, "Adding sensor from activeProgramA %d: Type=%s, Address=0x%02X, Capability=%s\n",
                  activeProgramA, it->sensorType.c_str(), it->sensorAddress, it->sensorCapability.c_str());
      addSensorIfUnique(it->sensorType, it->sensorAddress, it->sensorCapability, activeSensors);
    }
  }

  // Process activeProgramB
  if (activeProgramB != -1) {
    auto it = std::find_if(ProgramCache.begin(), ProgramCache.end(),
                           [&](const ProgramDetails &p) {
                             return p.id == activeProgramB;
                           });
    if (it != ProgramCache.end() && it->enabled && it->trigger == "Sensor" && !it->sensorType.isEmpty() && it->sensorAddress != 0 && !it->sensorCapability.isEmpty()) {
      DEBUG_PRINT(1, "Adding sensor from activeProgramB %d: Type=%s, Address=0x%02X, Capability=%s\n",
                  activeProgramB, it->sensorType.c_str(), it->sensorAddress, it->sensorCapability.c_str());
      addSensorIfUnique(it->sensorType, it->sensorAddress, it->sensorCapability, activeSensors);
    }
  }
  DEBUG_PRINT(1, "Updated activeSensors with %d sensors\n", activeSensors.size());
}

// Polling function for VEML6030
std::map<String, String> pollVEML6030(SensorInfo &sensor) {
  std::map<String, String> readings;
  for (const auto &cap : sensor.capabilities) {
    readings[cap] = "-1";
  }
  if (sensor.type != "VEML6030") {
    DEBUG_PRINT(1, "Invalid sensor type for VEML6030: %s\n", sensor.type.c_str());
    return readings;
  }
  static std::map<uint8_t, SparkFun_Ambient_Light> veml6030Sensors;
  static std::map<uint8_t, bool> initialized;
  i2cLock();
  if (!initialized[sensor.address]) {
    auto result = veml6030Sensors.emplace(sensor.address, SparkFun_Ambient_Light(sensor.address));
    if (!result.second) {
      result.first->second = SparkFun_Ambient_Light(sensor.address);
    }
    auto sensor_it = veml6030Sensors.find(sensor.address);
    if (sensor_it == veml6030Sensors.end()) {
      DEBUG_PRINT(1, "VEML6030 not found at 0x%02X after insertion\n", sensor.address);
      i2cUnlock();
      return readings;
    }
    if (!sensor_it->second.begin()) {
      DEBUG_PRINT(1, "VEML6030 initialization failed at 0x%02X\n", sensor.address);
      i2cUnlock();
      return readings;
    }
    DEBUG_PRINT(1, "VEML6030 initialized at 0x%02X\n", sensor.address);
    sensor_it->second.setGain(0.125);
    sensor_it->second.setIntegTime(100);
    initialized[sensor.address] = true;
  }
  auto sensor_it = veml6030Sensors.find(sensor.address);
  if (sensor_it == veml6030Sensors.end()) {
    DEBUG_PRINT(1, "VEML6030 not found at 0x%02X\n", sensor.address);
    i2cUnlock();
    return readings;
  }
  long lux = sensor_it->second.readLight();
  i2cUnlock();
  if (lux < 0) {
    DEBUG_PRINT(1, "VEML6030 read failed at 0x%02X\n", sensor.address);
    return readings;
  }
  char value[32];
  for (const auto &cap : sensor.capabilities) {
    if (cap == "Lux") {
      snprintf(value, sizeof(value), "%ld", lux);
      readings[cap] = value;
    }
  }
  return readings;
}

// Polling function for HDC2080
std::map<String, String> pollHDC2080(SensorInfo &sensor) {
  std::map<String, String> readings;
  for (const auto &cap : sensor.capabilities) {
    readings[cap] = "-1";
  }
  if (sensor.type != "HDC2080") {
    DEBUG_PRINT(1, "Invalid sensor type for HDC2080: %s\n", sensor.type.c_str());
    return readings;
  }
  static std::map<uint8_t, HDC2080> hdc2080Sensors;
  static std::map<uint8_t, bool> initialized;
  i2cLock();
  if (!initialized[sensor.address]) {
    auto result = hdc2080Sensors.emplace(sensor.address, HDC2080(sensor.address));
    if (!result.second) {
      result.first->second = HDC2080(sensor.address);
    }
    auto sensor_it = hdc2080Sensors.find(sensor.address);
    if (sensor_it == hdc2080Sensors.end()) {
      DEBUG_PRINT(1, "HDC2080 not found at 0x%02X after insertion\n", sensor.address);
      i2cUnlock();
      return readings;
    }
    sensor_it->second.begin();
    sensor_it->second.setMeasurementMode(TEMP_AND_HUMID);
    sensor_it->second.setRate(ONE_HZ);
    DEBUG_PRINT(1, "HDC2080 initialized at 0x%02X\n", sensor.address);
    initialized[sensor.address] = true;
  }
  auto sensor_it = hdc2080Sensors.find(sensor.address);
  if (sensor_it == hdc2080Sensors.end()) {
    DEBUG_PRINT(1, "HDC2080 not found at 0x%02X\n", sensor.address);
    i2cUnlock();
    return readings;
  }
  float temp = sensor_it->second.readTemp();
  float hum = sensor_it->second.readHumidity();
  i2cUnlock();
  if (isnan(temp) || isnan(hum)) {
    DEBUG_PRINT(1, "HDC2080 read failed at 0x%02X\n", sensor.address);
    return readings;
  }
  char value[32];
  for (const auto &cap : sensor.capabilities) {
    if (cap == "Temperature") {
      snprintf(value, sizeof(value), "%.1f", temp);
      readings[cap] = value;
    } else if (cap == "Humidity") {
      snprintf(value, sizeof(value), "%.1f", hum);
      readings[cap] = value;
    }
  }
  return readings;
}

// Polling function for MCP9600
std::map<String, String> pollMCP9600(SensorInfo &sensor) {
  std::map<String, String> readings;
  for (const auto &cap : sensor.capabilities) {
    readings[cap] = "-1";
  }
  if (sensor.type != "MCP9600") {
    DEBUG_PRINT(1, "Invalid sensor type for MCP9600: %s\n", sensor.type.c_str());
    return readings;
  }
  static std::map<uint8_t, MCP9600> mcp9600Sensors;
  static std::map<uint8_t, bool> initialized;
  i2cLock();
  if (!initialized[sensor.address]) {
    mcp9600Sensors.emplace(std::piecewise_construct,
                           std::forward_as_tuple(sensor.address),
                           std::forward_as_tuple(sensor.address));
    if (mcp9600Sensors[sensor.address].init(THER_TYPE_K) != NO_ERROR) {
      DEBUG_PRINT(1, "MCP9600 initialization failed at 0x%02X\n", sensor.address);
      i2cUnlock();
      return readings;
    }
    DEBUG_PRINT(1, "MCP9600 initialized at 0x%02X\n", sensor.address);
    initialized[sensor.address] = true;
  }
  float temp;
  if (mcp9600Sensors[sensor.address].read_hot_junc(&temp) != NO_ERROR) {
    DEBUG_PRINT(1, "MCP9600 read failed at 0x%02X\n", sensor.address);
    i2cUnlock();
    return readings;
  }
  i2cUnlock();
  char value[32];
  for (const auto &cap : sensor.capabilities) {
    if (cap == "Temperature") {
      snprintf(value, sizeof(value), "%.1f", temp);
      readings[cap] = value;
    }
  }
  return readings;
}

// Polling function for SHT3x
std::map<String, String> pollSHT3x(SensorInfo &sensor) {
  std::map<String, String> readings;
  for (const auto &cap : sensor.capabilities) {
    readings[cap] = "-1";
  }
  if (sensor.type != "SHT3x-DIS") {
    DEBUG_PRINT(1, "Invalid sensor type for SHT3x: %s\n", sensor.type.c_str());
    return readings;
  }
  static std::map<uint8_t, SensirionI2cSht3x> sht3xSensors;
  static std::map<uint8_t, bool> initialized;
  i2cLock();
  if (!initialized[sensor.address]) {
    sht3xSensors[sensor.address].begin(Wire, sensor.address);
    if (sht3xSensors[sensor.address].softReset() != NO_ERROR) {
      DEBUG_PRINT(1, "SHT3x initialization failed at 0x%02X\n", sensor.address);
      i2cUnlock();
      return readings;
    }
    DEBUG_PRINT(1, "SHT3x initialized at 0x%02X\n", sensor.address);
    initialized[sensor.address] = true;
  }
  float temp, hum;
  int16_t error = sht3xSensors[sensor.address].measureSingleShot(REPEATABILITY_HIGH, false, temp, hum);
  i2cUnlock();
  if (error != NO_ERROR) {
    DEBUG_PRINT(1, "SHT3x read failed at 0x%02X, error: 0x%04X\n", sensor.address, error);
    return readings;
  }
  char value[16];
  for (const auto &cap : sensor.capabilities) {
    if (cap == "Temperature") {
      snprintf(value, sizeof(value), "%.1f", temp);
      readings[cap] = value;
    } else if (cap == "Humidity") {
      snprintf(value, sizeof(value), "%.1f", hum);
      readings[cap] = value;
    }
  }
  return readings;
}

// Polling function for BME280
std::map<String, String> pollBME280(SensorInfo &sensor) {
  std::map<String, String> readings;
  for (const auto &cap : sensor.capabilities) {
    readings[cap] = "-1";
  }
  if (sensor.type != "BME280") {
    DEBUG_PRINT(1, "Invalid sensor type for BME280: %s\n", sensor.type.c_str());
    return readings;
  }
  static std::map<uint8_t, Adafruit_BME280> bme280Sensors;
  static std::map<uint8_t, bool> initialized;
  i2cLock();
  if (!initialized[sensor.address]) {
    if (!bme280Sensors[sensor.address].begin(sensor.address, &Wire)) {
      DEBUG_PRINT(1, "BME280 initialization failed at 0x%02X\n", sensor.address);
      i2cUnlock();
      return readings;
    }
    DEBUG_PRINT(1, "BME280 initialized at 0x%02X\n", sensor.address);
    initialized[sensor.address] = true;
  }
  float temp = bme280Sensors[sensor.address].readTemperature();
  float hum = bme280Sensors[sensor.address].readHumidity();
  float pres = bme280Sensors[sensor.address].readPressure() / 100.0F;
  i2cUnlock();
  if (isnan(temp) || isnan(hum) || isnan(pres)) {
    DEBUG_PRINT(1, "BME280 read failed at 0x%02X\n", sensor.address);
    return readings;
  }
  char value[16];
  for (const auto &cap : sensor.capabilities) {
    if (cap == "Temperature") {
      snprintf(value, sizeof(value), "%.1f", temp);
      readings[cap] = value;
    } else if (cap == "Humidity") {
      snprintf(value, sizeof(value), "%.1f", hum);
      readings[cap] = value;
    } else if (cap == "Pressure") {
      snprintf(value, sizeof(value), "%.1f", pres);
      readings[cap] = value;
    }
  }
  return readings;
}

// Placeholder for voltage reading (implement as needed)
int readVoltage(uint8_t address) {
  // TODO: Implement actual voltage reading for ACS71020
  return 230;  // Temporary placeholder
}

// Updated pollACS71020 for String consistency
std::map<String, String> pollACS71020(SensorInfo &sensor) {
  std::map<String, String> readings;
  for (const auto &cap : sensor.capabilities) {
    readings[cap] = "-1";
  }
  if (sensor.type != "ACS71020") {
    DEBUG_PRINT(1, "Invalid sensor type for ACS71020: %s\n", sensor.type.c_str());
    return readings;
  }
  const int maxRetries = 3;
  i2cLock();
  for (int attempt = 1; attempt <= maxRetries; attempt++) {
    Wire.beginTransmission(sensor.address);
    Wire.write(0x20);
    if (Wire.endTransmission() == 0) {
      Wire.requestFrom(sensor.address, (uint8_t)2);
      if (Wire.available() == 2) {
        uint16_t raw = (Wire.read() << 8) | Wire.read();
        i2cUnlock();
        float current = raw * 0.1f;
        int voltage = readVoltage(sensor.address);
        int power = static_cast<int>(current * voltage);
        char value[32];
        for (const auto &cap : sensor.capabilities) {
          if (cap == "Current") {
            snprintf(value, sizeof(value), "%.2f", round(current * 100.0f) / 100.0f);
            readings[cap] = value;
          } else if (cap == "Voltage") {
            snprintf(value, sizeof(value), "%d", voltage);
            readings[cap] = value;
          } else if (cap == "Power") {
            snprintf(value, sizeof(value), "%d", power);
            readings[cap] = value;
          }
        }
        return readings;
      }
    }
    i2cUnlock();
    delay(10);
    i2cLock();
  }
  i2cUnlock();
  DEBUG_PRINT(1, "ACS71020 read failed at 0x%02X\n", sensor.address);
  return readings;
}

// Main polling function
void pollSensors() {
  // Poll ACS71020 at 10Hz
  if (acsValid && millis() - lastACSPoll >= acsPollInterval) {
    for (auto &sensor : activeSensors) {
      if (sensor.type == "ACS71020") {
        auto readings = pollACS71020(sensor);
        ACSReading reading;
        reading.current = readings.at("Current") == "-1" ? -1.0f : atof(readings.at("Current").c_str());
        reading.voltage = readings.at("Voltage") == "-1" ? -1 : atoi(readings.at("Voltage").c_str());
        reading.power = readings.at("Power") == "-1" ? -1 : atoi(readings.at("Power").c_str());
        bool validReadings = (reading.current >= 0 && reading.voltage >= 0 && reading.power >= 0);
        if (!validReadings) {
          acsValid = false;
        }
        if (acsValid) {
          acsReadingsBuffer.push_back(reading);
        }
        lastACSPoll = millis();
      }
    }
  }

  // Update cache at 1Hz
  if (millis() - cacheUpdated < cacheUpdateInterval) {
    return;
  }

  // Create temporary cache
  std::map<String, String> newCache;

  // Process ACS71020 buffer
  if (acsValid && !acsReadingsBuffer.empty() && acsReadingsBuffer.size() > 0) {
    for (auto &sensor : activeSensors) {
      if (sensor.type == "ACS71020") {
        float currentSum = 0.0f;
        float voltageSum = 0.0f;
        float powerSum = 0.0f;
        size_t count = acsReadingsBuffer.size();
        for (const auto &reading : acsReadingsBuffer) {
          currentSum += reading.current;
          voltageSum += static_cast<float>(reading.voltage);
          powerSum += static_cast<float>(reading.power);
        }
        char value[32];
        for (const auto &cap : sensor.capabilities) {
          char key[64];
          snprintf(key, sizeof(key), "%s:0x%02X:%s", sensor.type.c_str(), sensor.address, cap.c_str());
          if (cap == "Current") {
            snprintf(value, sizeof(value), "%.2f", currentSum / count);
            newCache[key] = value;
          } else if (cap == "Voltage") {
            snprintf(value, sizeof(value), "%d", static_cast<int>(voltageSum / count));
            newCache[key] = value;
          } else if (cap == "Power") {
            snprintf(value, sizeof(value), "%d", static_cast<int>(powerSum / count));
            newCache[key] = value;
          }
        }
      }
    }
    acsReadingsBuffer.clear();
  } else {
    for (auto &sensor : activeSensors) {
      if (sensor.type == "ACS71020") {
        for (const auto &cap : sensor.capabilities) {
          char key[64];
          snprintf(key, sizeof(key), "%s:0x%02X:%s", sensor.type.c_str(), sensor.address, cap.c_str());
          newCache[key] = "-1";
        }
      }
    }
  }

  // Reset ACS71020 validation
  acsValid = true;

  // Poll other sensors
  for (auto &sensor : activeSensors) {
    if (sensor.type == "ACS71020") continue;
    std::map<String, String> readings;
    if (sensor.type == "VEML6030") {
      readings = pollVEML6030(sensor);
    } else if (sensor.type == "HDC2080") {
      readings = pollHDC2080(sensor);
    } else if (sensor.type == "MCP9600") {
      readings = pollMCP9600(sensor);
    } else if (sensor.type == "SHT3x-DIS") {
      readings = pollSHT3x(sensor);
    } else if (sensor.type == "BME280") {
      readings = pollBME280(sensor);
    } else {
      DEBUG_PRINT(1, "Unsupported sensor type: %s at 0x%02X\n", sensor.type.c_str(), sensor.address);
      for (const auto &cap : sensor.capabilities) {
        char key[64];
        snprintf(key, sizeof(key), "%s:0x%02X:%s", sensor.type.c_str(), sensor.address, cap.c_str());
        newCache[key] = "-1";
      }
      continue;
    }
    for (const auto &cap : sensor.capabilities) {
      char key[64];
      snprintf(key, sizeof(key), "%s:0x%02X:%s", sensor.type.c_str(), sensor.address, cap.c_str());
      newCache[key] = readings[cap];
    }
  }

  // Update main cache
  sensorReadingsCache = newCache;

  // Compare caches and send to WebSocket clients
  if (sensorReadingsCache != prevCache) {
    //sendToWSClients();
    prevCache = sensorReadingsCache;
    for (const auto &pair : sensorReadingsCache) {
      DEBUG_PRINT(2, "  %s: %s\n", pair.first.c_str(), pair.second.c_str());
    }
    sendActiveprograms();
  }
  // Update timestamp
  cacheUpdated = millis();
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
      digitalWrite(WIFI_LED, HIGH);
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
      digitalWrite(WIFI_LED, LOW);
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

void sendCycleTimerInfo(bool newClient = false) {
  static time_t lastNextCycleToggleA = 0;
  static time_t lastNextCycleToggleB = 0;

  // Skip change check for new clients or if subscriber_epochs is empty
  if (!newClient && !subscriber_epochs.empty()) {
    // Only send if toggle times have changed for non-new clients
    if (NextCycleToggleA == lastNextCycleToggleA && NextCycleToggleB == lastNextCycleToggleB) {
      return;  // No change, skip sending
    }
  }

  if (subscriber_epochs.empty()) {
    // Update stored values even if no subscribers to avoid sending stale data later
    lastNextCycleToggleA = NextCycleToggleA;
    lastNextCycleToggleB = NextCycleToggleB;
    return;
  }

  uint32_t new_epoch = millis();
  DynamicJsonDocument doc(256);  // Estimated size for small JSON
  doc["type"] = "cycle_timer_status";
  doc["epoch"] = new_epoch;
  doc["NextCycleToggleA"] = NextCycleToggleA;
  doc["NextCycleToggleB"] = NextCycleToggleB;
  doc["outputAState"] = outputAState;
  doc["outputBState"] = outputBState;

  char buffer[256];
  size_t len = serializeJson(doc, buffer, sizeof(buffer));
  if (len == 0) {
    Serial.println("sendCycleTimerInfo: JSON serialization failed");
    return;
  }

  for (auto &pair : subscriber_epochs) {
    if (newClient || new_epoch > pair.second) {
      pair.first->text(buffer);
      pair.second = new_epoch;
      DEBUG_PRINT(2, "Sent cycle_timer_status to client #%u\n", pair.first->id());
    }
  }
  DEBUG_PRINT(1, "Sent cycle_timer_status: NextCycleToggleA=%ld, NextCycleToggleB=%ld\n",
              NextCycleToggleA, NextCycleToggleB);

  // Update last sent values
  lastNextCycleToggleA = NextCycleToggleA;
  lastNextCycleToggleB = NextCycleToggleB;
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      DEBUG_PRINT(1, "WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
      {
        StaticJsonDocument<256> doc;
        doc["type"] = "time_offset";
        doc["offset_minutes"] = config.time_offset_minutes;
        doc["device_name"] = config.device_name;
        String json;
        serializeJson(doc, json);
        client->text(json);
      }
      subscriber_epochs[client] = 0;
      break;
    case WS_EVT_DISCONNECT:
      DEBUG_PRINT(2, "WebSocket client #%u disconnected\n", client->id());
      subscriber_epochs.erase(client);
      break;
    case WS_EVT_DATA:
      {
        char dataStr[512];
        size_t copyLen = min(len, sizeof(dataStr) - 1);
        memcpy(dataStr, data, copyLen);
        dataStr[copyLen] = '\0';  // Null-terminate
        DEBUG_PRINT(1, "WebSocket client #%u sent data: %s\n", client->id(), dataStr);
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, dataStr, len);
        if (!error) {
          const char *command = doc["command"].as<const char *>();
          if (command && strcmp(command, "subscribe_output_status") == 0) {
            subscriber_epochs[client] = 0;
            DEBUG_PRINT(1, "Client #%u subscribed to status\n", client->id());
            sendActiveprograms();
            sendDiscoveredSensors();
            sendProgramCache();
            sendCycleTimerInfo(true);
          } else if (command && strcmp(command, "unsubscribe_output_status") == 0) {
            subscriber_epochs.erase(client);
            DEBUG_PRINT(1, "Client #%u unsubscribed from status\n", client->id());
          } else if (command && strcmp(command, "get_output_status") == 0) {
            sendActiveprograms();
            sendCycleTimerInfo(true);
          } else if (command && strcmp(command, "get_discovered_sensors") == 0) {
            sendDiscoveredSensors();
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
          } else if (command && strcmp(command, "get_program_cache") == 0) {
            sendProgramCache();
          } else if (command && strcmp(command, "reset_A") == 0) {
            runCycleTimer("A", false, ProgramDetails{});
            DEBUG_PRINTLN(1, "Reset Cycle Timer A command recieved");
          } else if (command && strcmp(command, "reset_B") == 0) {
            runCycleTimer("B", false, ProgramDetails{});
            DEBUG_PRINTLN(1, "Reset Cycle Timer B command recieved");
          } else if (command && strcmp(command, "refresh-sensors") == 0) {
            requestSensorScan();
            sendDiscoveredSensors();
          } else if (command && strcmp(command, "set_time_offset") == 0) {
            int offset = 0;
            String device_name = "";
            if (doc["offset_minutes"].is<int>()) {
              offset = doc["offset_minutes"].as<int>();
            }
            if (doc["device_name"].is<String>()) {
              device_name = doc["device_name"].as<String>();
              if (device_name.length() > 25) {
                device_name = device_name.substring(0, 25);  // Limit to 25 characters
              }
            }
            if (offset >= -720 && offset <= 840) {
              config.time_offset_minutes = offset;
              strncpy(config.device_name, device_name.c_str(), 26);  // Ensure null-termination
              config.device_name[25] = '\0';                         // Enforce max length
              saveConfiguration("/config.json", config);
              DEBUG_PRINT(1, "Time offset updated to %d minutes, device name set to %s\n", offset, config.device_name);
              StaticJsonDocument<256> response;
              response["type"] = "time_offset";
              response["offset_minutes"] = config.time_offset_minutes;
              response["device_name"] = config.device_name;
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
  if (activeProgramA == -1) {
    outputAState = false;
    digitalWrite(OUTPUT_A_PIN, LOW);
  }
  if (activeProgramB == -1) {
    outputBState = false;
    digitalWrite(OUTPUT_B_PIN, LOW);
  }

  DEBUG_PRINT(1, "Programs set: activeProgramA=%d, activeProgramB=%d, nextTransA=%ld, nextTransB=%ld\n",
              activeProgramA, activeProgramB, nextTransA, nextTransB);
  updateActiveSensors();
}

bool runCycleTimer(const char *output, bool active, const ProgramDetails &prog) {
  static bool cycleARunning = false;
  static bool cycleAoutput = false;
  static bool cycleBRunning = false;
  static bool cycleBoutput = false;
  time_t now = time(nullptr);

  // Early validation check
  if (active && !prog.cycleConfig.valid) {
    DEBUG_PRINT(2, "ERROR: CycleTimer for %s invalid\n", output);
    if (strcmp(output, "A") == 0) {
      cycleAoutput = false;
      cycleARunning = false;
      NextCycleToggleA = 0;
    } else if (strcmp(output, "B") == 0) {
      cycleBoutput = false;
      cycleBRunning = false;
      NextCycleToggleB = 0;
    }
    sendCycleTimerInfo();  // Notify clients if toggle times changed
    return false;
  }

  if (strcmp(output, "A") == 0) {
    if (active) {
      if (!cycleARunning) {
        // Initialize cycle
        cycleAoutput = prog.cycleConfig.startHigh;
        cycleARunning = true;
        NextCycleToggleA = now + (cycleAoutput ? prog.cycleConfig.runSeconds : prog.cycleConfig.stopSeconds);
      } else if (now >= NextCycleToggleA) {
        // Toggle state
        cycleAoutput = !cycleAoutput;
        NextCycleToggleA = now + (cycleAoutput ? prog.cycleConfig.runSeconds : prog.cycleConfig.stopSeconds);
      }
    } else {
      // Stop cycle
      cycleAoutput = false;
      cycleARunning = false;
      NextCycleToggleA = 0;
    }
    sendCycleTimerInfo();  // Notify clients if toggle times changed
    return cycleAoutput;
  } else if (strcmp(output, "B") == 0) {
    if (active) {
      if (!cycleBRunning) {
        // Initialize cycle
        cycleBoutput = prog.cycleConfig.startHigh;
        cycleBRunning = true;
        NextCycleToggleB = now + (cycleBoutput ? prog.cycleConfig.runSeconds : prog.cycleConfig.stopSeconds);
      } else if (now >= NextCycleToggleB) {
        // Toggle state
        cycleBoutput = !cycleBoutput;
        NextCycleToggleB = now + (cycleBoutput ? prog.cycleConfig.runSeconds : prog.cycleConfig.stopSeconds);
      }
    } else {
      // Stop cycle
      cycleBoutput = false;
      cycleBRunning = false;
      NextCycleToggleB = 0;
    }
    sendCycleTimerInfo();  // Notify clients if toggle times changed
    return cycleBoutput;
  }

  DEBUG_PRINT(2, "ERROR: Invalid output %s\n", output);
  sendCycleTimerInfo();  // Notify clients if toggle times changed
  return false;
}

//update program cache from flash.
void updateProgramCache(const char *programID = nullptr) {
  DEBUG_PRINTLN(1, "Updating program cache...");
  ProgramCache.clear();
  null_program_ids.clear();

  time_t localEpoch = getAdjustedTime();

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
      details.name = doc["name"].as<String>();
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
        ProgramCache.push_back(details);
        DEBUG_PRINT(1, "Program %d: Added to null_program_ids and Program Cache\n", i);
      } else {
        DEBUG_PRINT(3, "Program %d: Loaded, Output=%s, Enabled=%d\n", i, details.output.c_str(), details.enabled);
        ProgramCache.push_back(details);
      }
    } else {
      DEBUG_PRINT(1, "Program %d: Failed to acquire LittleFS mutex\n", i);
    }
  }
  DEBUG_PRINTLN(1, "Program cache updated.");
  setProgramPriorities(localEpoch);
  sendProgramCache();
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
  DEBUG_PRINTLN(1, "Saving Program.");
  DEBUG_PRINT(1, "Recieved programID: ");
  DEBUG_PRINT(1, programID);
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
    updateProgramCache(programID);
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



void sendProgramCache() {
  if (subscriber_epochs.empty()) return;
  uint32_t new_epoch = millis();
  size_t jsonSize = ProgramCache.size() * 512 + 256;  // Increase estimate
  DynamicJsonDocument doc(jsonSize);
  doc["type"] = "program_cache";
  doc["epoch"] = new_epoch;
  JsonArray programs = doc.createNestedArray("programs");
  for (const auto &prog : ProgramCache) {
    JsonObject progObj = programs.createNestedObject();
    progObj["id"] = prog.id;
    progObj["name"] = prog.name;
    progObj["enabled"] = prog.enabled;
    progObj["output"] = prog.output;
    progObj["startDate"] = prog.startDate;
    progObj["startDateEnabled"] = prog.startDateEnabled;
    progObj["endDate"] = prog.endDate;
    progObj["endDateEnabled"] = prog.endDateEnabled;
    progObj["startTime"] = prog.startTime;
    progObj["startTimeEnabled"] = prog.startTimeEnabled;
    progObj["endTime"] = prog.endTime;
    progObj["endTimeEnabled"] = prog.endTimeEnabled;
    JsonArray days = progObj.createNestedArray("selectedDays");
    for (const auto &day : prog.selectedDays) {
      days.add(day);
    }
    progObj["daysPerWeekEnabled"] = prog.daysPerWeekEnabled;
    progObj["trigger"] = prog.trigger;
    progObj["sensorType"] = prog.sensorType;
    char addrStr[5];
    snprintf(addrStr, sizeof(addrStr), "0x%02X", prog.sensorAddress);
    progObj["sensorAddress"] = addrStr;
    progObj["sensorCapability"] = prog.sensorCapability;
    JsonObject cycle = progObj.createNestedObject("cycleConfig");
    cycle["runSeconds"] = prog.cycleConfig.runSeconds;
    cycle["stopSeconds"] = prog.cycleConfig.stopSeconds;
    cycle["startHigh"] = prog.cycleConfig.startHigh;
    cycle["valid"] = prog.cycleConfig.valid;
  }
  size_t requiredSize = measureJson(doc);
  if (requiredSize > 8192) {  // Increased buffer size
    Serial.println("ProgramCache JSON too large: " + String(requiredSize) + " bytes");
    return;
  }
  char buffer[8192];
  size_t len = serializeJson(doc, buffer, sizeof(buffer));
  if (len == 0) {
    Serial.println("ProgramCache JSON serialization failed");
    return;
  }
  DEBUG_PRINT(1, "Sending program_cache JSON (%d bytes): %s\n", len, buffer);
  for (auto &pair : subscriber_epochs) {
    if (new_epoch > pair.second) {
      pair.first->text(buffer);
      pair.second = new_epoch;
      DEBUG_PRINT(1, "Sent program_cache to client #%u\n", pair.first->id());
    }
  }
}

void updateOutputs() {
  outputAState = false;
  outputBState = false;

  if (activeProgramA > 0) {
    for (const auto &prog : ProgramCache) {
      if (prog.id == activeProgramA) {
        if (prog.trigger == "Cycle Timer") {
          outputAState = runCycleTimer("A", true, prog);
        } else if (prog.trigger == "Manual") {
          outputAState = true;
          runCycleTimer("A", false, prog);  // Stop any running cycle
        } else {
          runCycleTimer("A", false, prog);  // Stop any running cycle
          // Add sensor-based logic if needed
        }
        break;  // Found the program, no need to continue
      }
    }
  } else {
    runCycleTimer("A", false, ProgramDetails{});  // Stop cycle with default struct
  }

  if (activeProgramB > 0) {
    for (const auto &prog : ProgramCache) {
      if (prog.id == activeProgramB) {
        if (prog.trigger == "Cycle Timer") {
          outputBState = runCycleTimer("B", true, prog);
        } else if (prog.trigger == "Manual") {
          outputBState = true;
          runCycleTimer("B", false, prog);  // Stop any running cycle
        } else {
          runCycleTimer("B", false, prog);  // Stop any running cycle
          // Add sensor-based logic if needed
        }
        break;
      }
    }
  } else {
    runCycleTimer("B", false, ProgramDetails{});  // Stop cycle with default struct
  }

  digitalWrite(OUTPUT_A_PIN, outputAState ? HIGH : LOW);
  digitalWrite(OUTPUT_B_PIN, outputBState ? HIGH : LOW);
}

void printStatus() {
  Serial.println("** STATUS **");
  Serial.printf("Programs set: activeProgramA=%d, activeProgramB=%d, nextTransA=%ld, nextTransB=%ld\n",
                activeProgramA, activeProgramB, nextTransA, nextTransB);
  if (null_program_ids.empty()) {
    Serial.println("No Null programs");
  } else {
    Serial.print("Null Program IDs: [");
    for (size_t i = 0; i < null_program_ids.size(); ++i) {
      Serial.print(null_program_ids[i]);
      if (i < null_program_ids.size() - 1) {
        Serial.print(", ");
      }
    }
    Serial.println("]");
  }
  Serial.printf("activeSensors has %d sensors\n", activeSensors.size());
  for (const auto &sensor : activeSensors) {
    Serial.print("Type=");
    Serial.print(sensor.type.c_str());
    Serial.printf(", Address=0x%02X", sensor.address);
    Serial.printf(", PollingInterval=%lums", sensor.maxPollingInterval);
    Serial.print(", Capabilities=[");
    for (size_t i = 0; i < sensor.capabilities.size(); ++i) {
      Serial.print(sensor.capabilities[i].c_str());
      if (i < sensor.capabilities.size() - 1) {
        Serial.print(", ");
      }
    }
    Serial.println("]");
  }
  for (const auto &pair : sensorReadingsCache) {
    Serial.print("Sensor Reading:  ");
    Serial.print(pair.first.c_str());
    Serial.print(": ");
    Serial.println(pair.second.c_str());
  }
}

void updateDebugLevel() {
  if (Serial.available() > 0) {
    String input = Serial.readStringUntil('\n');  // Read until newline
    input.trim();                                 // Remove whitespace
    if (input.length() > 0) {
      if (input.equalsIgnoreCase("status")) {
        printStatus();  // Call status function
      } else {
        // Try to convert to int
        bool isNumber = true;
        for (int i = 0; i < input.length(); i++) {
          if (!isDigit(input[i])) {
            isNumber = false;
            break;
          }
        }

        if (isNumber) {
          int newLevel = input.toInt();
          if (newLevel >= 0 && newLevel <= 3) {  // Validate range
            debugLevel = newLevel;
            DEBUG_PRINT(0, "Debug level set to %d\n", debugLevel);
          } else {
            Serial.println("Invalid debug level. Use 0-3.");
          }
        } else {
          Serial.println("Invalid input. Use 0-3 or 'status'.");
        }
      }
    }
  }
}
void standardizeProgramIDs() {
  if (xSemaphoreTake(littlefsMutex, portTICK_PERIOD_MS * 1000) == pdTRUE) {
    for (int i = 1; i < 10; i++) {
      String oldFilename = "/program" + String(i) + ".json";
      String newFilename = "/program0" + String(i) + ".json";

      // Check if old file exists and new file does not
      if (LittleFS.exists(oldFilename) && !LittleFS.exists(newFilename)) {
        if (LittleFS.rename(oldFilename, newFilename)) {
          DEBUG_PRINT(1, "Renamed %s to %s\n", oldFilename.c_str(), newFilename.c_str());

          // Update ProgramCache
          for (auto &prog : ProgramCache) {
            if (prog.id == i) {
              prog.id = i;  // ID remains the same, but ensure consistency
              DEBUG_PRINT(2, "Updated ProgramCache ID %d for %s\n", prog.id, newFilename.c_str());
            }
          }

          // Update null_program_ids
          for (auto it = null_program_ids.begin(); it != null_program_ids.end(); ++it) {
            if (*it == i) {
              *it = i;  // ID remains the same, but ensure consistency
              DEBUG_PRINT(2, "Updated null_program_ids ID %d\n", i);
            }
          }
        } else {
          DEBUG_PRINT(1, "Failed to rename %s to %s\n", oldFilename.c_str(), newFilename.c_str());
        }
      }
    }
    xSemaphoreGive(littlefsMutex);
  } else {
    DEBUG_PRINT(1, "Failed to acquire LittleFS mutex for standardizeProgramIDs\n");
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

  i2cMutex = xSemaphoreCreateMutex();
  if (i2cMutex == NULL) {
    Serial.println("Failed to create I2C mutex");
    return;
  }

  // Initialize I2C
  Wire.begin(I2C_SDA, I2C_SCL);
  Serial.println("I2C initialized");

  // Create FreeRTOS task for I2C scanning
  xTaskCreatePinnedToCore(i2cScanTask, "I2CScan", 4096, NULL, 1, NULL, 0);

  WiFi.onEvent(networkEvent);
  if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED)) {
    Serial.println("LittleFS Mount Failed");
    return;
  }
  readSettings();

  standardizeProgramIDs();

  //pinMode(PHY_PWR, OUTPUT);
  //digitalWrite(PHY_PWR, HIGH);
  pinMode(OUTPUT_A_PIN, OUTPUT);
  pinMode(OUTPUT_B_PIN, OUTPUT);
  pinMode(WIFI_BTN, INPUT);
  pinMode(WIFI_LED, OUTPUT);
  pinMode(INPUT_A, INPUT);
  pinMode(INPUT_B, INPUT);
  pinMode(IO34, INPUT);  //Remote Button
  //pinMode(IO36, INPUT); // Unused IO
  pinMode(IO2, OUTPUT);   //RED LED
  pinMode(IO15, OUTPUT);  //LEDs Enable
  digitalWrite(OUTPUT_A_PIN, LOW);
  digitalWrite(OUTPUT_B_PIN, LOW);
  digitalWrite(WIFI_LED, HIGH);
  digitalWrite(IO2, HIGH);
  digitalWrite(IO15, HIGH);
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
  //ElegantOTA.begin(&server, OTA_USERNAME, OTA_PASSWORD);
  server.begin();
  updateProgramCache();
  digitalWrite(WIFI_LED, LOW);
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
    DEBUG_PRINT(1, "UTC Time: %s", timeStr);
    if (wifi_connected) {
      DEBUG_PRINT(1, " WiFi IP address: %s", WiFi.localIP().toString().c_str());
    } else if (eth_connected) {
      DEBUG_PRINT(1, " Ethernet IP: %s", ETH.localIP().toString().c_str());
    }
    DEBUG_PRINTLN(1, " Free heap: %lu", ESP.getFreeHeap());
    print_time_count = millis();
  }
  if (shouldCheckInternet() == 1) {
    checkInternetConnectivity();
  }
  if (sensorsChanged) {
    DEBUG_PRINT(1, "Sensors changed, sending to clients. ");
    sendDiscoveredSensors();
    sensorsChanged = false;
  }
  static unsigned long lastTimeSend = 0;
  static unsigned long lastOutputUpdate = 0;
  if (millis() - lastOutputUpdate >= 100) {
    if (timeSynced) {
      time_t localEpoch = getAdjustedTime();
      if (nextTransA != -1 && localEpoch >= nextTransA) {
        DEBUG_PRINTLN(1, "nextTransA has passed, updating priorities");
        setProgramPriorities(localEpoch);
      }
      if (nextTransB != -1 && localEpoch >= nextTransB) {
        DEBUG_PRINTLN(1, "nextTransB has passed, updating priorities");
        setProgramPriorities(localEpoch);
      }
      updateOutputs();
    }
    pollSensors();
    lastOutputUpdate = millis();
    if (millis() - lastTimeSend >= 1000) {
      sendTimeToClients();
      lastTimeSend = millis();
      bool readA = digitalRead(INPUT_A);
      if (readA) {
        DEBUG_PRINTLN(1, "Input A Pin High");
      }
      bool readB = digitalRead(INPUT_B);
      if (readB) {
        DEBUG_PRINTLN(1, "Input B Pin High");
      }
      static bool readIO34 = false;
      readIO34 =  digitalRead(IO34);
      if (readIO34) {
        digitalWrite(IO15, HIGH);
        DEBUG_PRINTLN(1, "Input IO34 Pin High"); //Remote Push Button
      } else digitalWrite(IO15, LOW);
    }
  }
  //ElegantOTA.loop();
}