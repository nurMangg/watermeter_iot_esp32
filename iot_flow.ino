#include <WiFi.h>
#include <LittleFS.h>
#include <BluetoothSerial.h>
#include <time.h>

BluetoothSerial SerialBT;

// Default WiFi Credentials (hanya untuk sync waktu)
String wifiSSID = "Redmi";
String wifiPassword = "komet123";

// Customer ID
String customerID = "PAM0001";

// Pin sensor
#define SENSOR_PIN 18

// File paths
const char* CONFIG_PATH = "/config.txt";
const char* LOG_PATH = "/log.json";
const char* TIME_SYNC_FLAG = "/timesync.txt";

// Sensor calibration
const float ML_PER_PULSE = 2.25f / 1000.0f;

// Intervals
const unsigned long MEASURE_INTERVAL = 1000;
const unsigned long FLUSH_INTERVAL = 30000;     // Reduced to 30s
const unsigned long BT_SEND_INTERVAL = 2000;
const unsigned long WIFI_TIMEOUT = 20000;       // 20 detik timeout untuk sync waktu
const int BUFFER_SIZE = 5;

// Runtime variables
volatile unsigned int pulseCount = 0;
float totalVolume = 0.0f;
float currentFlowRate = 0.0f;

// Buffer for log entries
String dataBuffer[BUFFER_SIZE];
int bufferIndex = 0;

// Connection states
bool timeSynced = false;
bool wifiDisconnected = false;
bool clientConnected = false;
bool customerIDSent = false;

// Timing variables
unsigned long lastMeasure = 0;
unsigned long lastFlush = 0;
unsigned long lastBTSend = 0;
unsigned long wifiStartTime = 0;

// Base timestamp for relative time calculation
unsigned long baseTimestamp = 0;
unsigned long systemStartTime = 0;

// ISR for sensor pulses
void IRAM_ATTR onPulse() {
  pulseCount++;
}

// Save WiFi config to LittleFS
bool saveWifiConfig(const String &ssid, const String &pass) {
  File f = LittleFS.open(CONFIG_PATH, FILE_WRITE);
  if (!f) return false;
  f.println(ssid);
  f.println(pass);
  f.close();
  return true;
}

// Load WiFi config from LittleFS
void loadWifiConfig() {
  if (!LittleFS.exists(CONFIG_PATH)) return;
  
  File f = LittleFS.open(CONFIG_PATH, FILE_READ);
  if (!f) return;
  
  String ssid = f.readStringUntil('\n');
  String pass = f.readStringUntil('\n');
  f.close();
  
  ssid.trim();
  pass.trim();
  
  if (ssid.length() > 0) {
    wifiSSID = ssid;
    wifiPassword = pass;
  }
}

// Check if time was previously synced
bool wasTimeSynced() {
  return LittleFS.exists(TIME_SYNC_FLAG);
}

// Mark time as synced
void markTimeSynced() {
  File f = LittleFS.open(TIME_SYNC_FLAG, FILE_WRITE);
  if (f) {
    f.println("synced");
    f.close();
  }
}

// One-time WiFi connection for time sync only
void syncTimeOnce() {
  if (wasTimeSynced()) {
    Serial.println("[TIME] Time already synced before, skipping WiFi");
    timeSynced = true;
    systemStartTime = millis();
    return;
  }
  
  if (wifiSSID.length() == 0) {
    Serial.println("[TIME] No WiFi credentials, using system time");
    timeSynced = true;
    systemStartTime = millis();
    return;
  }
  
  Serial.println("[WIFI] Connecting for time sync: " + wifiSSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  wifiStartTime = millis();
  
  // Wait for connection or timeout
  while (WiFi.status() != WL_CONNECTED && 
         (millis() - wifiStartTime) < WIFI_TIMEOUT) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WIFI] Connected for time sync");
    
    // Sync time via NTP
    configTime(25200, 0, "pool.ntp.org", "time.nist.gov");
    
    // Wait for time sync (max 10 seconds)
    int retries = 0;
    struct tm timeinfo;
    while (!getLocalTime(&timeinfo) && retries < 20) {
      delay(500);
      retries++;
    }
    
    if (getLocalTime(&timeinfo)) {
      Serial.println("[TIME] NTP sync successful");
      baseTimestamp = time(nullptr);
      systemStartTime = millis();
      markTimeSynced();
      timeSynced = true;
    } else {
      Serial.println("[TIME] NTP sync failed, using system time");
      systemStartTime = millis();
      timeSynced = true;
    }
    
    // Disconnect WiFi after sync
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[WIFI] Disconnected after time sync");
    wifiDisconnected = true;
    
  } else {
    Serial.println("\n[WIFI] Connection failed, using system time");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    systemStartTime = millis();
    timeSynced = true;
    wifiDisconnected = true;
  }
}

// Get current timestamp (real time or estimated)
time_t getCurrentTimestamp() {
  if (baseTimestamp > 0) {
    // Calculate current time based on synced base + elapsed system time
    unsigned long elapsedSeconds = (millis() - systemStartTime) / 1000;
    return baseTimestamp + elapsedSeconds;
  } else {
    // Fallback: use system uptime as timestamp
    return systemStartTime / 1000 + (millis() - systemStartTime) / 1000;
  }
}

// Get formatted datetime
String getDateTime() {
  time_t currentTime = getCurrentTimestamp();
  struct tm *timeinfo = localtime(&currentTime);
  
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M:%S", timeinfo);
  return String(buffer);
}

// Simple log append (efficient for standalone operation)
void appendLog(const String &entry) {
  File file = LittleFS.open(LOG_PATH, FILE_APPEND);
  if (!file) {
    // Create new file
    file = LittleFS.open(LOG_PATH, FILE_WRITE);
    if (file) {
      file.println("[");
      file.println(entry + ",");
    }
  } else {
    file.println(entry + ",");
  }
  
  if (file) {
    file.close();
  }
}

// Send log file via Bluetooth
void sendLogFile() {
  if (!LittleFS.exists(LOG_PATH)) {
    SerialBT.println("[CMD] No log file found");
    return;
  }
  
  File file = LittleFS.open(LOG_PATH, FILE_READ);
  if (!file) {
    SerialBT.println("[CMD] Failed to open log");
    return;
  }
  
  SerialBT.println("[LOG_START]");
  
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() > 0 && !line.equals("[") && !line.equals("]")) {
      if (line.endsWith(",")) {
        line = line.substring(0, line.length() - 1);
      }
      if (line.startsWith("{")) {
        SerialBT.println(line);
        delay(10); // Prevent buffer overflow
      }
    }
  }
  
  file.close();
  SerialBT.println("[LOG_END]");
}

// Flush buffer to storage
void flushBuffer() {
  if (bufferIndex == 0) return;
  
  for (int i = 0; i < bufferIndex; i++) {
    appendLog(dataBuffer[i]);
  }
  
  Serial.println("[BUFFER] Flushed " + String(bufferIndex) + " entries");
  bufferIndex = 0;
}

// Process Bluetooth commands
void processBTCommand(const String &cmd) {
  Serial.println("[BT] CMD: " + cmd);
  
  if (cmd == "RESET_LOG") {
    LittleFS.remove(LOG_PATH);
    SerialBT.println("[CMD] Log reset");
    
  } else if (cmd == "RESET_TOTAL") {
    totalVolume = 0.0f;
    SerialBT.println("[CMD] Total volume reset");
    
  } else if (cmd == "RESET_ALL") {
    LittleFS.remove(LOG_PATH);
    totalVolume = 0.0f;
    bufferIndex = 0;
    SerialBT.println("[CMD] All data reset");
    
  } else if (cmd == "RESET_TIME_SYNC") {
    LittleFS.remove(TIME_SYNC_FLAG);
    SerialBT.println("[CMD] Time sync flag reset - will sync on next restart");
    
  } else if (cmd.startsWith("SET_WIFI:")) {
    String params = cmd.substring(9);
    int commaIndex = params.indexOf(',');
    
    if (commaIndex > 0) {
      String newSSID = params.substring(0, commaIndex);
      String newPassword = params.substring(commaIndex + 1);
      
      newSSID.trim();
      newPassword.trim();
      
      if (saveWifiConfig(newSSID, newPassword)) {
        wifiSSID = newSSID;
        wifiPassword = newPassword;
        SerialBT.println("[CMD] WiFi updated: " + newSSID);
        SerialBT.println("[CMD] Restart device to sync time with new WiFi");
      } else {
        SerialBT.println("[CMD] Failed to save WiFi config");
      }
    } else {
      SerialBT.println("[CMD] Format: SET_WIFI:ssid,password");
    }
    
  } else if (cmd == "GET_LOG") {
    sendLogFile();
    
  } else if (cmd == "STATUS") {
    SerialBT.println("[STATUS] Time Synced: " + String(timeSynced ? "YES" : "NO"));
    SerialBT.println("[STATUS] WiFi: DISCONNECTED (by design)");
    SerialBT.println("[STATUS] Total: " + String(totalVolume, 2) + "L");
    SerialBT.println("[STATUS] Flow: " + String(currentFlowRate, 2) + "L/min");
    SerialBT.println("[STATUS] Buffer: " + String(bufferIndex) + "/" + String(BUFFER_SIZE));
    SerialBT.println("[STATUS] Uptime: " + String(millis()/1000) + "s");
    
  } else if (cmd == "GET_TIME") {
    SerialBT.println("[TIME] Current: " + getDateTime());
    
  } else {
    SerialBT.println("[CMD] Unknown: " + cmd);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n[SYSTEM] Water Meter IoT Starting...");
  
  // Initialize LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("[ERROR] LittleFS failed!");
    while(1) delay(1000);
  }
  
  // Load WiFi config
  loadWifiConfig();
  
  // Initialize Bluetooth (always on)
  SerialBT.begin("WaterMeter-IoT");
  Serial.println("[BT] Bluetooth ready");
  
  // One-time WiFi connection for time sync only
  syncTimeOnce();
  
  // Initialize sensor
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), onPulse, RISING);
  Serial.println("[SENSOR] Flow sensor ready on pin " + String(SENSOR_PIN));
  
  Serial.println("[SYSTEM] Ready - WiFi OFF, Bluetooth ON");
  Serial.println("[INFO] Current time: " + getDateTime());
}

void loop() {
  unsigned long now = millis();
  
  // Measure flow rate
  if (now - lastMeasure >= MEASURE_INTERVAL) {
    lastMeasure = now;
    
    // Read pulses atomically
    noInterrupts();
    unsigned int pulses = pulseCount;
    pulseCount = 0;
    interrupts();
    
    if (pulses > 0) {
      float litres = pulses * ML_PER_PULSE;
      currentFlowRate = (litres / (MEASURE_INTERVAL / 1000.0f)) * 60.0f;
      totalVolume += litres;
      
      // Create log entry
      String entry = "{\"datetime\":\"" + getDateTime() + "\"," +
                    "\"total\":" + String(totalVolume, 3) + "," +
                    "\"flow\":" + String(currentFlowRate, 2) + "}";
      
      // Add to buffer
      if (bufferIndex < BUFFER_SIZE) {
        dataBuffer[bufferIndex++] = entry;
      } else {
        flushBuffer();
        dataBuffer[bufferIndex++] = entry;
      }
      
    } else {
      currentFlowRate = 0.0f;
    }
  }
  
  // Periodic flush
  if (now - lastFlush >= FLUSH_INTERVAL) {
    lastFlush = now;
    flushBuffer();
  }
  
  // Handle Bluetooth connections
  bool hasClient = SerialBT.hasClient();
  if (hasClient && !clientConnected) {
    clientConnected = true;
    customerIDSent = false;
    Serial.println("[BT] Client connected");
  } else if (!hasClient && clientConnected) {
    clientConnected = false;
    Serial.println("[BT] Client disconnected");
  }
  
  // Send customer ID on new connection
  if (clientConnected && !customerIDSent) {
    SerialBT.println("CustomerID:" + customerID);
    customerIDSent = true;
  }
  
  // Process Bluetooth commands
  if (clientConnected && SerialBT.available()) {
    String cmd = SerialBT.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) {
      processBTCommand(cmd);
    }
  }
  
  // Send data via Bluetooth
  if (clientConnected && now - lastBTSend >= BT_SEND_INTERVAL) {
    lastBTSend = now;
    String msg = "FlowRate:" + String(currentFlowRate, 2) + 
                "L/min,Total:" + String(totalVolume, 2) + 
                "L,Time:" + getDateTime();
    SerialBT.println(msg);
  }
  
  delay(10);
}
