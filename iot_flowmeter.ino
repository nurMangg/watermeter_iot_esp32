#include <WiFi.h>
#include <LittleFS.h>
#include <BluetoothSerial.h>
#include <time.h>

BluetoothSerial SerialBT;

// Default WiFi Credentials (overridable via Bluetooth)
String wifiSSID     = "Wifi eMang";
String wifiPassword = "indonesia";

// Customer ID
String customerID = "PAM0001";

// Pin sensor
#define SENSOR_PIN 18

// File paths
typedef const char* cstr;
cstr CONFIG_PATH = "/config.txt";
cstr LOG_PATH    = "/log.json";

// Sensor calibration
const float ML_PER_PULSE = 2.25f / 1000.0f;

// Intervals
const unsigned long MEASURE_INTERVAL = 1000;
const unsigned long FLUSH_INTERVAL   = 120000;
const int BUFFER_SIZE                = 5;

// Runtime variables
volatile unsigned int pulseCount = 0;
float totalVolume    = 0.0f;
float currentFlowRate = 0.0f;

// Buffer for log entries
String dataBuffer[BUFFER_SIZE];
int bufferIndex = 0;

bool clientConnected = false;

// ISR for sensor pulses
void IRAM_ATTR onPulse() {
  pulseCount++;
}

// Save WiFi config to LittleFS
void saveWifiConfig(const String &ssid, const String &pass) {
  File f = LittleFS.open(CONFIG_PATH, FILE_WRITE);
  if (!f) return;
  f.println(ssid);
  f.println(pass);
  f.close();
}

// Load WiFi config from LittleFS if exists
void loadWifiConfig() {
  if (!LittleFS.exists(CONFIG_PATH)) return;
  File f = LittleFS.open(CONFIG_PATH, FILE_READ);
  if (!f) return;
  String s = f.readStringUntil('\n');
  String p = f.readStringUntil('\n');
  f.close();
  if (s.length()) wifiSSID = s;
  if (p.length()) wifiPassword = p;
}

// SNTP time initialization
void setupTime() {
  configTime(25200, 0, "pool.ntp.org");
  struct tm ti;
  if (!getLocalTime(&ti)) Serial.println("[TIME] NTP sinkron gagal");
}

// Get formatted datetime
String getDateTime() {
  struct tm ti;
  if (!getLocalTime(&ti)) return "00/00/0000 00:00:00";
  char buf[20];
  sprintf(buf, "%02d/%02d/%04d %02d:%02d:%02d",
          ti.tm_mday, ti.tm_mon+1, ti.tm_year+1900,
          ti.tm_hour, ti.tm_min, ti.tm_sec);
  return String(buf);
}

// Append log entry into JSON array in LittleFS
void appendLog(const String &entry) {
  if (!LittleFS.exists(LOG_PATH)) {
    File f = LittleFS.open(LOG_PATH, FILE_WRITE);
    if (f) { f.print("[]"); f.close(); }
  }
  File r = LittleFS.open(LOG_PATH, FILE_READ);
  if (!r) return;
  String c = r.readString(); r.close(); c.trim();
  if (c.length()<2 || !c.startsWith("[") || !c.endsWith("]")) c = "[]";
  c = c.substring(0, c.length()-1);
  if (c.length()>1) c += ",\n";
  c += entry + "]";
  File w = LittleFS.open(LOG_PATH, FILE_WRITE);
  if (!w) return;
  w.print(c); w.flush(); w.close();
}

// Send log file to Bluetooth client
void sendLogFile() {
  if (!LittleFS.exists(LOG_PATH)) {
    SerialBT.println("[CMD] Log file tidak ditemukan");
    return;
  }
  
  File f = LittleFS.open(LOG_PATH, FILE_READ);
  if (!f) {
    SerialBT.println("[CMD] Gagal membuka log file");
    return;
  }
  
  String content = f.readString();
  f.close();
  
  // Parse JSON array
  if (content.length() < 2 || !content.startsWith("[") || !content.endsWith("]")) {
    SerialBT.println("[CMD] Format log tidak valid");
    return;
  }
  
  // Remove brackets
  content = content.substring(1, content.length() - 1);
  
  // Split by commas that are followed by newlines (or just commas if no newlines)
  int startPos = 0;
  int commaPos = content.indexOf(",\n");
  if (commaPos == -1) commaPos = content.indexOf(",");
  
  while (commaPos != -1) {
    String entry = content.substring(startPos, commaPos);
    entry.trim();
    if (entry.length() > 0) {
      SerialBT.println("[LOG] " + entry);
      delay(10); // Small delay to avoid buffer overflows
    }
    
    startPos = commaPos + 1;
    if (content.charAt(commaPos + 1) == '\n') startPos++;
    
    commaPos = content.indexOf(",\n", startPos);
    if (commaPos == -1) commaPos = content.indexOf(",", startPos);
  }
  
  // Handle the last entry
  if (startPos < content.length()) {
    String entry = content.substring(startPos);
    entry.trim();
    if (entry.length() > 0) {
      SerialBT.println("[LOG] " + entry);
    }
  }
  
  SerialBT.println("[CMD] Log data terkirim");
}

// Flush buffered entries
void flushBuffer() {
  if (!bufferIndex) return;
  for (int i=0; i<bufferIndex; ++i) appendLog(dataBuffer[i]);
  bufferIndex = 0;
}

// Process commands via Bluetooth
void processBTCommand(const String &cmd) {
  if (cmd == "RESET_LOG") {
    LittleFS.remove(LOG_PATH);
    SerialBT.println("[CMD] Log di-reset");
  } else if (cmd == "RESET_TOTAL") {
    totalVolume = 0.0f;
    SerialBT.println("[CMD] Total volume di-reset");
  } else if (cmd == "RESET_ALL") {
    LittleFS.remove(LOG_PATH);
    totalVolume = 0.0f;
    bufferIndex = 0;
    SerialBT.println("[CMD] Semua data di-reset");
  } else if (cmd.startsWith("SET_WIFI:")) {
    String p = cmd.substring(9);
    int i = p.indexOf(',');
    if (i>0) {
      String ns = p.substring(0,i);
      String np = p.substring(i+1);
      saveWifiConfig(ns,np);
      wifiSSID = ns; wifiPassword = np;
      SerialBT.println("[CMD] WiFi di-update: " + ns);
      WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
    } else SerialBT.println("[CMD] Format SET_WIFI salah");
  } else if (cmd == "GET_LOG") {
    sendLogFile();
  } else {
    SerialBT.println("[CMD] Unknown: " + cmd);
  }
}

void setup() {
  Serial.begin(115200);
  if (!LittleFS.begin(true)) { while(1) delay(1000); }
  loadWifiConfig();
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
  while (WiFi.status()!=WL_CONNECTED) { delay(500); }
  setupTime();
  SerialBT.begin("WaterMeter-IoT");
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), onPulse, RISING);
}

void loop() {
  static unsigned long prevMeasure = 0;
  static unsigned long prevFlush   = 0;
  static unsigned long prevSend    = 0;
  static bool customerIDSent = false;

  unsigned long now = millis();

  // Hitung flowrate dan buffer
  if (now - prevMeasure >= MEASURE_INTERVAL) {
    prevMeasure = now;
    unsigned int pulses = pulseCount;
    pulseCount = 0;

    float litres = pulses * ML_PER_PULSE;
    currentFlowRate = (litres / (MEASURE_INTERVAL / 1000.0f)) * 60.0f;
    totalVolume += litres;

    if (litres > 0.0f) {
      String entry = "{\"datetime\":\"" + getDateTime() + "\",";
      entry += "\"flowRate\":" + String(currentFlowRate, 2) + "}";
      entry += "\"total\":" + String(totalVolume,2) + "}";
      dataBuffer[bufferIndex++] = entry;
      if (bufferIndex >= BUFFER_SIZE) flushBuffer();
    }
  }

  // Emergency flush
  if (now - prevFlush >= FLUSH_INTERVAL) {
    prevFlush = now;
    flushBuffer();
  }

  // Bluetooth status
  bool hasClient = SerialBT.hasClient();
  if (hasClient && !clientConnected) {
    clientConnected = true;
    customerIDSent = false;  // Reset flag when new client connects
    Serial.println("[BT] Client terhubung");
  } else if (!hasClient && clientConnected) {
    clientConnected = false;
    Serial.println("[BT] Client terputus");
  }

  // Send customer ID when Bluetooth connects (only once per connection)
  if (clientConnected && !customerIDSent) {
    SerialBT.println("CustomerID:" + customerID);
    Serial.println("[BT] Sent CustomerID: " + customerID);
    customerIDSent = true;  // Set flag to prevent repeated sending
  }

  // Terima perintah via Bluetooth
  if (clientConnected && SerialBT.available()) {
    String cmd = SerialBT.readStringUntil('\n');
    cmd.trim();
    processBTCommand(cmd);
  }

  // Kirim data via Bluetooth
  if (clientConnected && now - prevSend >= 2000) {
    prevSend = now;
    String msg = "FlowRate:" + String(currentFlowRate,2) + "/Lmin, Total:" + String(totalVolume,2) + "L";
    SerialBT.println(msg);
    Serial.println("[BT] Sent: " + msg);
  }

  delay(10);
}