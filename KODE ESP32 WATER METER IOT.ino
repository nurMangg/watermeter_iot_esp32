#include <WiFi.h>
#include <LittleFS.h>
#include <BluetoothSerial.h>

BluetoothSerial SerialBT;

// Konfigurasi dasar
String wifiSSID = "Redmi";
String wifiPassword = "komet123";
String customerID = "PAM0001";

#define SENSOR_PIN 18

// Konstanta yang sangat konservatif
const float ML_PER_PULSE = 2.25f / 1000.0f;
const unsigned long MEASURE_INTERVAL = 3000;   // 3 detik
const unsigned long SAVE_INTERVAL = 60000;    // 5 menit
const unsigned long BT_SEND_INTERVAL = 5000;   // 5 detik

// NTP Configuration
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 25200;     // GMT+7 (Indonesia) = 7 * 3600
const int daylightOffset_sec = 0;

// Variables dengan inisialisasi yang aman
volatile unsigned int pulseCount = 0;
float totalVolume = 0.0f;
float currentFlowRate = 0.0f;
bool clientConnected = false;
bool systemReady = false;
bool timeInitialized = false;

// Buffer data sederhana
struct DataEntry {
  float volume;
  time_t timestamp; 

};

DataEntry currentData = {0.0f, 0};
bool hasNewData = false;

// ISR yang sangat minimal dan aman
void IRAM_ATTR onPulse() {
  pulseCount++;
}

// Fungsi untuk mendapatkan epoch time
time_t getEpochTime() {
  if (!timeInitialized) {
    return millis() / 1000; // Fallback ke millis jika NTP belum ready
  }
  
  time_t now;
  time(&now);  // Proper way to get current time
  
  if (now < 1000000000) { // Validasi: timestamp harus > 1 billion (tahun 2001)
    return millis() / 1000; // Fallback jika timestamp tidak valid
  }
  
  return now;
}

// Fungsi untuk format timestamp ke string readable
String formatTimestamp(time_t epochTime) {
  if (!timeInitialized || epochTime < 1000000000) {
    return String((unsigned long)epochTime); // Return epoch jika NTP belum ready
  }
  
  struct tm *timeinfo = localtime(&epochTime);  // Fixed: proper pointer usage
  if (timeinfo == nullptr) {
    return String((unsigned long)epochTime);
  }
  
  char buffer[20];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
  return String(buffer);
}

// Initialize NTP time
void initTime() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TIME] WiFi not connected, using millis");
    return;
  }
  
  Serial.println("[TIME] Initializing NTP...");
  
  // Configure NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // Wait for time to be set with better checking
  Serial.print("[TIME] Waiting for NTP sync");
  unsigned long startTime = millis();
  time_t now = 0;
  
  while (now < 1000000000 && millis() - startTime < 15000) { // Increased timeout
    time(&now);
    delay(500);
    Serial.print(".");
  }
  
  if (now > 1000000000) {
    timeInitialized = true;
    Serial.println();
    Serial.println("[TIME] NTP synchronized successfully!");
    Serial.println("[TIME] Current time: " + formatTimestamp(now));
  } else {
    Serial.println();
    Serial.println("[TIME] NTP failed, using millis fallback");
    timeInitialized = false;
  }
}

// Sync NTP periodically (non-blocking)
void syncNTP() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TIME] WiFi disconnected, cannot sync NTP");
    return;
  }
  
  Serial.println("[TIME] Syncing NTP...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // Wait a bit for sync
  delay(2000);
  
  time_t now;
  time(&now);
  
  if (now > 1000000000) {
    timeInitialized = true;
    Serial.println("[TIME] NTP sync successful: " + formatTimestamp(now));
  } else {
    Serial.println("[TIME] NTP sync failed");
  }
}

// Fungsi aman untuk membaca file
bool safeReadFile(const char* path, String& content) {
  if (!LittleFS.exists(path)) return false;
  
  File file = LittleFS.open(path, FILE_READ);
  if (!file) return false;
  
  content = file.readString();
  file.close();
  return true;
}

// Fungsi aman untuk menulis file
bool safeWriteFile(const char* path, const String& content) {
  File file = LittleFS.open(path, FILE_WRITE);
  if (!file) return false;
  
  file.print(content);
  file.close();
  return true;
}

// Load total volume dengan safety check
void loadTotalVolume() {
  String content;
  if (safeReadFile("/total.txt", content)) {
    content.trim();
    if (content.length() > 0) {
      totalVolume = content.toFloat();
      Serial.println("[LOAD] Total: " + String(totalVolume, 3) + "L");
    }
  }
}

// Save total volume dengan safety check
void saveTotalVolume() {
  String content = String(totalVolume, 3);
  if (safeWriteFile("/total.txt", content)) {
    Serial.println("[SAVE] Total saved: " + content + "L");
  }
}

// Load WiFi config dengan safety check
void loadWifiConfig() {
  String content;
  if (safeReadFile("/config.txt", content)) {
    int newlinePos = content.indexOf('\n');
    if (newlinePos > 0) {
      String ssid = content.substring(0, newlinePos);
      String pass = content.substring(newlinePos + 1);
      
      ssid.trim();
      pass.trim();
      
      if (ssid.length() > 0) {
        wifiSSID = ssid;
        wifiPassword = pass;
        Serial.println("[LOAD] WiFi config loaded: " + ssid);
      }
    }
  }
}

// Save data log dengan format sederhana
void saveDataLog() {
  if (!hasNewData) return;
  
  time_t realTime = getEpochTime();
  String logEntry = String((unsigned long)realTime) + "," + String(currentData.volume, 3) + "\n";
  
  // Append ke file log
  File file = LittleFS.open("/log.csv", FILE_APPEND);
  if (file) {
    file.print(logEntry);
    file.close();
    Serial.println("[LOG] Data saved");
  }
  
  hasNewData = false;
}

// Command processor yang sangat sederhana
void processBTCommand(const String& cmd) {
  Serial.println("[CMD] " + cmd);
  
  if (cmd == "RESET_LOG") {
    LittleFS.remove("/log.csv");
    SerialBT.println("OK:LOG_RESET");
    
  } else if (cmd == "RESET_TOTAL") {
    totalVolume = 0.0f;
    LittleFS.remove("/total.txt");
    SerialBT.println("OK:TOTAL_RESET");
    
  } else if (cmd == "RESET_ALL") {
    totalVolume = 0.0f;
    LittleFS.remove("/total.txt");
    LittleFS.remove("/log.csv");
    SerialBT.println("OK:ALL_RESET");
    
  } else if (cmd.startsWith("SET_WIFI:")) {
    String params = cmd.substring(9);
    int commaPos = params.indexOf(',');
    if (commaPos > 0) {
      String newSSID = params.substring(0, commaPos);
      String newPass = params.substring(commaPos + 1);
      
      String config = newSSID + "\n" + newPass;
      if (safeWriteFile("/config.txt", config)) {
        SerialBT.println("OK:WIFI_SET");
      } else {
        SerialBT.println("ERROR:WIFI_SET");
      }
    }
    
  } else if (cmd == "GET_STATUS") {
    String status = "FLOW:" + String(currentFlowRate, 1) + 
                   ",TOTAL:" + String(totalVolume, 2) + 
                   ",WIFI:" + (WiFi.status() == WL_CONNECTED ? "OK" : "NO") +
                    ",TIME:" + (timeInitialized ? formatTimestamp(getEpochTime()) : "NO");
    SerialBT.println(status);
  } else if (cmd == "GET_TIME") {
    SerialBT.println("TIME:" + formatTimestamp(getEpochTime()));
    
  } else if (cmd == "SYNC_TIME") {
    syncNTP();
    SerialBT.println("OK:TIME_SYNC");
  } else if (cmd == "GET_LOG") {
    // Kirim log dalam format CSV sederhana
    String content;
    if (safeReadFile("/log.csv", content)) {
      if (content.length() > 2000) {
        SerialBT.println("ERROR:LOG_TOO_LARGE");
      } else {
        SerialBT.println("LOG_START");
        SerialBT.print(content);
        SerialBT.println("LOG_END");
      }
    }  
    else {
      SerialBT.println("ERROR:NO_LOG");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n[SETUP] Starting...");

  // Mount LittleFS dengan error handling
  if (!LittleFS.begin(true)) {
    Serial.println("[ERROR] LittleFS failed");
    ESP.restart();
  }
  Serial.println("[SETUP] LittleFS OK");

  // Load data
  loadTotalVolume();
  loadWifiConfig();

  // Init Bluetooth dengan nama sederhana
  if (!SerialBT.begin("WaterMeter")) {
    Serial.println("[ERROR] Bluetooth failed");
    ESP.restart();
  }
  Serial.println("[SETUP] Bluetooth OK");

  // Setup sensor dengan pull-up
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  delay(100);
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), onPulse, FALLING);
  Serial.println("[SETUP] Sensor OK");

  // WiFi connection (optional)
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 5000) {
    delay(250);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[SETUP] WiFi connected: " + WiFi.localIP().toString());
    delay(1000);
    initTime();
  } else {
    Serial.println("\n[SETUP] WiFi failed, continuing...");
  }

  systemReady = true;
  Serial.println("[SETUP] System ready");
}

void loop() {
  if (!systemReady) {
    delay(100);
    return;
  }

  static unsigned long lastMeasure = 0;
  static unsigned long lastSave = 0;
  static unsigned long lastSend = 0;
  static bool idSent = false;
  
  unsigned long now = millis();
  
  // Handle millis() overflow (setiap 49 hari)
  if (now < lastMeasure) {
    lastMeasure = now;
    lastSave = now;
    lastSend = now;
  }
  
  // Check BT connection
  bool hasClient = SerialBT.hasClient();
  if (hasClient && !clientConnected) {
    clientConnected = true;
    idSent = false;
    Serial.println("[BT] Connected");
    delay(100); // Stabilize connection
  } else if (!hasClient && clientConnected) {
    clientConnected = false;
    Serial.println("[BT] Disconnected");
  }

  // Send customer ID once per connection
  if (clientConnected && !idSent) {
    SerialBT.println("ID:" + customerID);
    idSent = true;
    delay(50);
  }

  // Measure flow
  if (now - lastMeasure >= MEASURE_INTERVAL) {
    lastMeasure = now;
    
    // Get pulse count safely
    noInterrupts();
    unsigned int pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    if (pulses > 0) {
      float litres = pulses * ML_PER_PULSE;
      currentFlowRate = (litres / (MEASURE_INTERVAL / 1000.0f)) * 60.0f;
      totalVolume += litres;

      // Store data for logging
      currentData.volume = totalVolume;
      currentData.timestamp = getEpochTime();
      hasNewData = true;

      Serial.println("[FLOW] Rate: " + String(currentFlowRate, 2) + " L/min, Total: " + String(totalVolume, 3) + "L");
    } else {
      currentFlowRate = 0.0f;
    }
  }

  // Save data periodically
  if (now - lastSave >= SAVE_INTERVAL) {
    lastSave = now;
    saveDataLog();
    saveTotalVolume();
  }

  // Handle BT commands
  if (clientConnected && SerialBT.available()) {
    String cmd = SerialBT.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0 && cmd.length() < 100) { // Limit command length
      processBTCommand(cmd);
    }
  }

  // Send data to BT client
  if (clientConnected && now - lastSend >= BT_SEND_INTERVAL) {
    lastSend = now;
    String msg = "DATA:" + String(currentFlowRate, 1) + "," + String(totalVolume, 2);
    SerialBT.println(msg);
  }

  // Watchdog dan yield
  yield();
  delay(100);
}
