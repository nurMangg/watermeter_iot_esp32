#include <WiFi.h>
#include <LittleFS.h>
#include <BluetoothSerial.h>
#include <time.h>

BluetoothSerial SerialBT;

// Default WiFi Credentials (overridable via Bluetooth)
String wifiSSID      = "Redmi";
String wifiPassword  = "komet123";

// Customer ID
String customerID = "PAM0001";

// Pin sensor
#define SENSOR_PIN 18

// File paths
typedef const char* cstr;
cstr CONFIG_PATH = "/config.txt";
cstr LOG_PATH    = "/log.json";

// Sensor calibration
const float ML_PER_PULSE = 2.25f / 1000.0f; // 2.25 mL per pulse

// Intervals - diperpanjang untuk mengurangi beban
const unsigned long MEASURE_INTERVAL = 2000;  // 2 detik (dari 1 detik)
const unsigned long FLUSH_INTERVAL   = 300000; // 5 menit (dari 2 menit)
const unsigned long BT_SEND_INTERVAL = 3000;  // 3 detik (dari 2 detik)
const int BUFFER_SIZE                = 10;     // Diperbesar dari 5

// Runtime variables
volatile unsigned int pulseCount = 0;
volatile unsigned long lastPulseTime = 0;
float totalVolume   = 0.0f;
float currentFlowRate = 0.0f;

// Buffer for log entries
String dataBuffer[BUFFER_SIZE];
int bufferIndex = 0;

bool clientConnected = false;
bool wifiConnected = false;

// Debounce untuk sensor
const unsigned long DEBOUNCE_TIME = 50; // 50ms debounce

// ISR untuk sensor pulses - optimized dengan debounce
void IRAM_ATTR onPulse() {
  unsigned long currentTime = millis();
  if (currentTime - lastPulseTime > DEBOUNCE_TIME) {
    pulseCount++;
    lastPulseTime = currentTime;
  }
}

// Save WiFi config to LittleFS
void saveWifiConfig(const String &ssid, const String &pass) {
  File f = LittleFS.open(CONFIG_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("[FS] Gagal membuka config.txt untuk ditulis");
    return;
  }
  f.println(ssid);
  f.println(pass);
  f.close();
  Serial.println("[FS] Konfigurasi WiFi disimpan: SSID=" + ssid);
}

// Load WiFi config from LittleFS if exists
void loadWifiConfig() {
  if (!LittleFS.exists(CONFIG_PATH)) {
    Serial.println("[FS] config.txt tidak ditemukan. Menggunakan kredensial WiFi default.");
    return;
  }
  File f = LittleFS.open(CONFIG_PATH, FILE_READ);
  if (!f) {
    Serial.println("[FS] Gagal membuka config.txt untuk dibaca");
    return;
  }
  String s = f.readStringUntil('\n'); s.trim();
  String p = f.readStringUntil('\n'); p.trim();
  f.close();

  if (s.length() > 0) {
    wifiSSID = s;
    Serial.println("[FS] SSID WiFi dimuat dari config: " + wifiSSID);
  }
  wifiPassword = p;
  if (s.length() > 0) {
    Serial.println("[FS] Password WiFi dimuat dari config.");
  }
}

// SNTP time initialization - non-blocking
void setupTime() {
  if (WiFi.status() == WL_CONNECTED) {
    configTime(25200, 0, "pool.ntp.org"); // GMT+7 (WIB)
    Serial.println("[TIME] Konfigurasi NTP dimulai (non-blocking).");
  } else {
    Serial.println("[TIME] WiFi tidak terhubung, sinkronisasi NTP dilewati.");
  }
}

// Get formatted datetime dengan error handling yang lebih baik
String getDateTime() {
  struct tm ti;
  if (!getLocalTime(&ti, 1000)) { // Timeout diperkecil menjadi 1 detik
    return "00/00/0000 00:00:00";
  }
  char buf[20];
  snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d:%02d",
           ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900,
           ti.tm_hour, ti.tm_min, ti.tm_sec);
  return String(buf);
}

// Optimized append log dengan error handling yang lebih baik
void appendLog(const String &entry) {
  // Cek ukuran entry untuk mencegah memory overflow
  if (entry.length() > 200) {
    Serial.println("[LOG] Entry terlalu panjang, diabaikan");
    return;
  }

  if (!LittleFS.exists(LOG_PATH)) {
    File f = LittleFS.open(LOG_PATH, FILE_WRITE);
    if (f) {
      f.print("[]");
      f.close();
    } else {
      Serial.println("[FS] Gagal membuat log.json awal");
      return;
    }
  }

  File r = LittleFS.open(LOG_PATH, FILE_READ);
  if (!r) {
    Serial.println("[FS] Gagal membuka log.json untuk dibaca");
    return;
  }
  
  String content = r.readString();
  r.close();
  
  // Cek ukuran file untuk mencegah file terlalu besar
  if (content.length() > 50000) { // 50KB limit
    Serial.println("[LOG] File log terlalu besar, mereset");
    content = "[]";
  }
  
  content.trim();
  if (content.length() < 2 || !content.startsWith("[") || !content.endsWith("]")) {
    Serial.println("[LOG] Format log tidak valid, mereset");
    content = "[]";
  }

  content = content.substring(0, content.length() - 1);
  if (content.length() > 1 && content != "[") {
    content += ",\n";
  }
  content += entry + "]";

  File w = LittleFS.open(LOG_PATH, FILE_WRITE);
  if (w) {
    w.print(content);
    w.close();
  }
}

// Send log file dengan chunking untuk mencegah buffer overflow
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

  // Baca file dalam chunks untuk menghindari memory overflow
  const size_t CHUNK_SIZE = 1024;
  char buffer[CHUNK_SIZE];
  
  SerialBT.println("[CMD] Mulai mengirim log...");
  
  while (f.available()) {
    size_t bytesRead = f.readBytes(buffer, CHUNK_SIZE - 1);
    buffer[bytesRead] = '\0';
    SerialBT.print(buffer);
    
    // Delay untuk mencegah buffer overflow Bluetooth
    delay(100);
    
    // Cek apakah client masih terhubung
    if (!SerialBT.hasClient()) {
      Serial.println("[BT] Client terputus saat mengirim log");
      break;
    }
  }
  
  f.close();
  SerialBT.println("\n[CMD] Log data terkirim");
}

// Optimized flush buffer
void flushBuffer() {
  if (bufferIndex == 0) return;
  
  Serial.printf("[LOG] Flushing buffer: %d entries\n", bufferIndex);
  
  for (int i = 0; i < bufferIndex; i++) {
    appendLog(dataBuffer[i]);
    yield(); // Berikan kesempatan untuk task lain
  }
  bufferIndex = 0;

  // Simpan total volume
  File f = LittleFS.open("/total.txt", FILE_WRITE);
  if (f) {
    f.print(String(totalVolume, 3));
    f.close();
    Serial.printf("[FS] Total volume disimpan: %.3f L\n", totalVolume);
  }
}

// Process BT commands dengan timeout
void processBTCommand(const String &cmd) {
  Serial.println("[BT CMD] Received: " + cmd);
  
  if (cmd == "RESET_LOG") {
    if (LittleFS.remove(LOG_PATH)) {
      SerialBT.println("[CMD] Log di-reset");
    } else {
      SerialBT.println("[CMD] Gagal mereset log");
    }
  } 
  else if (cmd == "RESET_TOTAL") {
    totalVolume = 0.0f;
    LittleFS.remove("/total.txt");
    SerialBT.println("[CMD] Total volume di-reset");
  } 
  else if (cmd == "RESET_ALL") {
    LittleFS.remove("/total.txt");
    LittleFS.remove(LOG_PATH);
    totalVolume = 0.0f;
    bufferIndex = 0;
    SerialBT.println("[CMD] Semua data di-reset");
  } 
  else if (cmd.startsWith("SET_WIFI:")) {
    String params = cmd.substring(9);
    int commaIndex = params.indexOf(',');
    if (commaIndex > 0) {
      String newSSID = params.substring(0, commaIndex);
      String newPass = params.substring(commaIndex + 1);
      saveWifiConfig(newSSID, newPass);
      wifiSSID = newSSID;
      wifiPassword = newPass;
      SerialBT.println("[CMD] WiFi di-update: " + newSSID);
      
      // Reconnect WiFi di background
      WiFi.disconnect();
      WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
    } else {
      SerialBT.println("[CMD] Format SET_WIFI salah. Gunakan SET_WIFI:ssid,password");
    }
  } 
  else if (cmd == "GET_LOG") {
    sendLogFile();
  } 
  else if (cmd == "STATUS") {
    SerialBT.printf("[CMD] WiFi: %s, Total: %.3fL, Flow: %.2fL/min\n", 
                   WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected",
                   totalVolume, currentFlowRate);
  }
  else {
    SerialBT.println("[CMD] Unknown: " + cmd);
  }
}

void loadTotalVolume() {
  if (!LittleFS.exists("/total.txt")) {
    Serial.println("[FS] File total.txt tidak ditemukan. Inisialisasi ke 0.");
    totalVolume = 0.0f;
    return;
  }
  
  File f = LittleFS.open("/total.txt", FILE_READ);
  if (!f) {
    Serial.println("[FS] Gagal membuka total.txt");
    totalVolume = 0.0f;
    return;
  }
  
  String val = f.readString();
  f.close();
  val.trim();
  totalVolume = val.toFloat();
  Serial.printf("[FS] Total volume dimuat: %.3f L\n", totalVolume);
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n[SETUP] Memulai perangkat...");

  // Inisialisasi LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("[SETUP] LittleFS Mount Gagal!");
    while (1) delay(1000);
  }
  Serial.println("[SETUP] LittleFS berhasil di-mount.");

  // Load total volume dan config
  loadTotalVolume();
  loadWifiConfig();

  // Inisialisasi Bluetooth dengan buffer yang lebih besar
  Serial.println("[SETUP] Menginisialisasi Bluetooth...");
  SerialBT.begin("WaterMeter-IoT");
  Serial.println("[SETUP] Bluetooth aktif: WaterMeter-IoT");

  // Setup sensor pin dan interrupt
  pinMode(SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), onPulse, FALLING); // Gunakan FALLING untuk stabilitas
  Serial.println("[SETUP] Sensor pin dikonfigurasi.");

  // WiFi connection (non-blocking)
  Serial.println("[SETUP] Mencoba WiFi: " + wifiSSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

  // Setup waktu
  setupTime();

  Serial.println("[SETUP] Setup selesai. Sistem berjalan.");
}

void loop() {
  static unsigned long prevMeasure = 0;
  static unsigned long prevFlush = 0;
  static unsigned long prevSend = 0;
  static unsigned long prevWiFiCheck = 0;
  static bool customerIDSent = false;

  unsigned long now = millis();

  // Cek status WiFi secara berkala (setiap 30 detik)
  if (now - prevWiFiCheck >= 30000) {
    prevWiFiCheck = now;
    wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected) {
      Serial.println("[WiFi] Status: Connected, IP: " + WiFi.localIP().toString());
    } else {
      Serial.println("[WiFi] Status: Disconnected");
    }
  }

  // Pengukuran flow rate
  if (now - prevMeasure >= MEASURE_INTERVAL) {
    prevMeasure = now;
    
    // Baca pulse count secara atomik
    noInterrupts();
    unsigned int pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    if (pulses > 0) {
      float litres = (float)pulses * ML_PER_PULSE;
      // Hitung flow rate per menit
      currentFlowRate = (litres / (MEASURE_INTERVAL / 1000.0f)) * 60.0f;
      totalVolume += litres;

      // Buat entry log
      String entry = "{\"datetime\":\"" + getDateTime() + "\",";
      entry += "\"total\":" + String(totalVolume, 3) + ",";
      entry += "\"flow\":" + String(currentFlowRate, 2) + "}";
      
      // Tambah ke buffer
      if (bufferIndex < BUFFER_SIZE) {
        dataBuffer[bufferIndex++] = entry;
      }
      
      // Auto flush jika buffer penuh
      if (bufferIndex >= BUFFER_SIZE) {
        flushBuffer();
      }
      
      Serial.printf("[MEASURE] Pulses: %d, Flow: %.2f L/min, Total: %.3f L\n", 
                    pulses, currentFlowRate, totalVolume);
    } else {
      currentFlowRate = 0.0f;
    }
  }

  // Flush buffer berkala
  if (now - prevFlush >= FLUSH_INTERVAL) {
    prevFlush = now;
    if (bufferIndex > 0) {
      Serial.println("[LOG] Interval flush...");
      flushBuffer();
    }
  }

  // Handle Bluetooth connection
  bool hasClient = SerialBT.hasClient();
  if (hasClient && !clientConnected) {
    clientConnected = true;
    customerIDSent = false;
    Serial.println("[BT] Client terhubung");
  } else if (!hasClient && clientConnected) {
    clientConnected = false;
    Serial.println("[BT] Client terputus");
  }

  // Kirim Customer ID sekali per koneksi
  if (clientConnected && !customerIDSent) {
    SerialBT.println("CustomerID:" + customerID);
    Serial.println("[BT] CustomerID terkirim: " + customerID);
    customerIDSent = true;
  }

  // Terima command Bluetooth
  if (clientConnected && SerialBT.available()) {
    String cmd = SerialBT.readStringUntil('\n');
    cmd.trim();
    if (cmd.length() > 0) {
      processBTCommand(cmd);
    }
  }

  // Kirim data berkala via Bluetooth
  if (clientConnected && (now - prevSend >= BT_SEND_INTERVAL)) {
    prevSend = now;
    
    // Format data yang lebih ringkas
    String msg = "Data:" + String(currentFlowRate, 2) + "," + String(totalVolume, 3);
    SerialBT.println(msg);
  }

  // Yield untuk mencegah watchdog timeout
  yield();
  delay(10);
}
