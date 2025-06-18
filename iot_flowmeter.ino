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

// Intervals
const unsigned long MEASURE_INTERVAL = 1000;  // 1 detik
const unsigned long FLUSH_INTERVAL   = 120000; // 2 menit
const int BUFFER_SIZE                = 5;

// Runtime variables
volatile unsigned int pulseCount = 0;
float totalVolume   = 0.0f;
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
  } else {
    Serial.println("[FS] SSID kosong di config.txt. Menggunakan SSID default.");
  }
  // Password bisa kosong untuk jaringan terbuka
  // Selalu update password jika ada di file, bahkan jika kosong
  wifiPassword = p;
  if (s.length() > 0) { // Hanya tampilkan pesan jika SSID juga dimuat, agar tidak membingungkan
    Serial.println("[FS] Password WiFi dimuat dari config.");
  }
}

// SNTP time initialization
void setupTime() {
  // Hanya coba konfigurasi waktu jika WiFi terhubung
  if (WiFi.status() == WL_CONNECTED) {
    configTime(25200, 0, "pool.ntp.org"); // GMT+7 (WIB)
    struct tm ti;
    if (!getLocalTime(&ti)) {
      Serial.println("[TIME] Sinkronisasi NTP gagal meskipun WiFi terhubung.");
    } else {
      Serial.println("[TIME] Sinkronisasi NTP berhasil.");
    }
  } else {
    Serial.println("[TIME] WiFi tidak terhubung, sinkronisasi NTP dilewati.");
  }
}

// Get formatted datetime
String getDateTime() {
  struct tm ti;
  if (!getLocalTime(&ti, 5000)) { // Tambahkan timeout kecil untuk getLocalTime
    // Jika gagal mendapatkan waktu (misalnya NTP belum sinkron), kembalikan placeholder
    return "00/00/0000 00:00:00";
  }
  char buf[20];
  sprintf(buf, "%02d/%02d/%04d %02d:%02d:%02d",
          ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900,
          ti.tm_hour, ti.tm_min, ti.tm_sec);
  return String(buf);
}

// Append log entry into JSON array in LittleFS
void appendLog(const String &entry) {
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
    Serial.println("[FS] Gagal membuka log.json untuk dibaca (append)");
    return;
  }
  String c = r.readString();
  r.close();
  c.trim();

  if (c.length() < 2 || !c.startsWith("[") || !c.endsWith("]")) {
    Serial.println("[LOG] Format log.json tidak valid, mereset ke []");
    c = "[]"; // Reset ke array kosong jika format tidak sesuai
  }

  // Hapus ']' terakhir
  c = c.substring(0, c.length() - 1);

  // Tambahkan koma jika sudah ada entri sebelumnya
  if (c.length() > 1 && c != "[") { // Pastikan bukan hanya "["
    c += ",\n";
  }
  c += entry + "]";

  File w = LittleFS.open(LOG_PATH, FILE_WRITE); // Mode FILE_WRITE akan menimpa file
  if (!w) {
    Serial.println("[FS] Gagal membuka log.json untuk ditulis (append)");
    return;
  }
  w.print(c);
  w.flush();
  w.close();
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

  if (content.length() < 2 || !content.startsWith("[") || !content.endsWith("]")) {
    SerialBT.println("[CMD] Format log tidak valid");
    return;
  }

  // Hapus tanda kurung siku offenbar
  content = content.substring(1, content.length() - 1);
  content.trim(); // Hapus spasi ekstra jika ada

  // Jika konten kosong setelah menghapus kurung (misalnya "[]")
  if (content.length() == 0) {
    SerialBT.println("[LOG] Log kosong.");
    SerialBT.println("[CMD] Log data terkirim");
    return;
  }
  
  int startPos = 0;
  int braceCount = 0;
  String entry = "";

  for (int i = 0; i < content.length(); i++) {
    char currentChar = content.charAt(i);
    if (currentChar == '{') {
      if (braceCount == 0) {
        startPos = i;
      }
      braceCount++;
    } else if (currentChar == '}') {
      braceCount--;
      if (braceCount == 0) {
        entry = content.substring(startPos, i + 1);
        entry.trim();
        if (entry.length() > 0) {
          SerialBT.println("[LOG] " + entry);
          delay(20); // Delay kecil untuk menghindari buffer overflow Bluetooth
        }
      }
    }
  }
  SerialBT.println("[CMD] Log data terkirim");
}

// Flush buffered entries
void flushBuffer() {
  if (!bufferIndex) return;
  Serial.print("[LOG] Flushing buffer: "); Serial.println(bufferIndex);
  for (int i = 0; i < bufferIndex; ++i) {
    appendLog(dataBuffer[i]);
  }
  bufferIndex = 0;
}

// Process commands via Bluetooth
void processBTCommand(const String &cmd) {
  Serial.println("[BT CMD] Received: " + cmd);
  if (cmd == "RESET_LOG") {
    if (LittleFS.remove(LOG_PATH)) {
      SerialBT.println("[CMD] Log di-reset");
    } else {
      SerialBT.println("[CMD] Gagal mereset log");
    }
  } else if (cmd == "RESET_TOTAL") {
    totalVolume = 0.0f;
    SerialBT.println("[CMD] Total volume di-reset");
  } else if (cmd == "RESET_ALL") {
    LittleFS.remove(LOG_PATH);
    totalVolume = 0.0f;
    bufferIndex = 0; // Juga reset buffer
    SerialBT.println("[CMD] Semua data di-reset");
  } else if (cmd.startsWith("SET_WIFI:")) {
    String p = cmd.substring(9);
    int i = p.indexOf(',');
    if (i > 0) {
      String ns = p.substring(0, i);
      String np = p.substring(i + 1);
      saveWifiConfig(ns, np);
      wifiSSID = ns;
      wifiPassword = np;
      SerialBT.println("[CMD] WiFi di-update: " + ns + ". Menyambungkan ulang...");
      WiFi.disconnect(); // Putuskan koneksi lama
      WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str()); // Coba sambungkan dengan kredensial baru
    } else {
      SerialBT.println("[CMD] Format SET_WIFI salah. Harusnya SET_WIFI:ssid,password");
    }
  } else if (cmd == "GET_LOG") {
    sendLogFile();
  } else {
    SerialBT.println("[CMD] Unknown: " + cmd);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n[SETUP] Memulai perangkat...");

  // Inisialisasi LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("[SETUP] LittleFS Mount Gagal. Perangkat berhenti.");
    while (1) { delay(1000); } // Berhenti jika LittleFS gagal
  }
  Serial.println("[SETUP] LittleFS berhasil di-mount.");

  loadWifiConfig(); // Muat konfigurasi WiFi dari LittleFS

  // Inisialisasi Bluetooth SEBELUM mencoba koneksi WiFi
  Serial.println("[SETUP] Menginisialisasi Bluetooth...");
  SerialBT.begin("WaterMeter-IoT");
  Serial.println("[SETUP] Bluetooth aktif, nama: WaterMeter-IoT. Menunggu koneksi...");

  // Coba hubungkan ke WiFi (non-blocking)
  Serial.println("[SETUP] Mencoba menghubungkan ke WiFi: " + wifiSSID);
  WiFi.mode(WIFI_STA); // Set mode ke Station
  WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());

  unsigned long wifiConnectStartTime = millis();
  bool wifiConnectedInSetup = false;
  Serial.print("[SETUP] Menunggu koneksi WiFi (maks 10 detik): ");
  while (WiFi.status() != WL_CONNECTED && (millis() - wifiConnectStartTime < 10000)) {
    Serial.print(".");
    delay(500);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectedInSetup = true;
    Serial.println("[SETUP] WiFi terhubung!");
    Serial.print("[SETUP] Alamat IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[SETUP] WiFi tidak dapat terhubung dalam 10 detik. Melanjutkan tanpa WiFi.");
  }
  
  // Setup waktu (NTP). Akan gagal jika WiFi tidak terhubung, getDateTime() akan mengembalikan default.
  setupTime(); 

  pinMode(SENSOR_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), onPulse, RISING);
  Serial.println("[SETUP] Sensor pin dan interrupt dikonfigurasi.");

  Serial.println("[SETUP] Setup selesai. Sistem berjalan.");
}

void loop() {
  static unsigned long prevMeasure = 0;
  static unsigned long prevFlush   = 0;
  static unsigned long prevSend    = 0;
  static bool customerIDSent = false; // Pindah ke sini agar statusnya persisten antar koneksi client

  unsigned long now = millis();

  // Hitung flowrate dan buffer data
  if (now - prevMeasure >= MEASURE_INTERVAL) {
    prevMeasure = now;
    
    // Ambil pulseCount secara atomik dan reset
    noInterrupts();
    unsigned int pulses = pulseCount;
    pulseCount = 0;
    interrupts();

    if (pulses > 0) { // Hanya proses jika ada pulse
      float litres = (float)pulses * ML_PER_PULSE; // Konversi ke float untuk kalkulasi
      // Flow rate dihitung per detik, lalu dikonversi ke liter per menit
      currentFlowRate = (litres / (MEASURE_INTERVAL / 1000.0f)) * 60.0f; 
      totalVolume += litres;

      String entry = "{\"datetime\":\"" + getDateTime() + "\",";
      entry += "\"total\":" + String(totalVolume, 3) + "}"; // presisi 3 desimal untuk volume
      
      if (bufferIndex < BUFFER_SIZE) {
          dataBuffer[bufferIndex++] = entry;
      }
      if (bufferIndex >= BUFFER_SIZE) {
        flushBuffer();
      }
    } else {
      currentFlowRate = 0.0f; // Tidak ada pulse, flow rate 0
    }
  }

  // Emergency flush jika buffer belum penuh tapi sudah waktunya
  if (now - prevFlush >= FLUSH_INTERVAL) {
    prevFlush = now;
    if (bufferIndex > 0) { // Hanya flush jika ada data di buffer
        Serial.println("[LOG] Interval flush tercapai.");
        flushBuffer();
    }
  }

  // Cek status koneksi Bluetooth
  bool hasClient = SerialBT.hasClient();
  if (hasClient && !clientConnected) {
    clientConnected = true;
    customerIDSent = false; // Reset flag saat client baru terhubung
    Serial.println("[BT] Client terhubung");
  } else if (!hasClient && clientConnected) {
    clientConnected = false;
    Serial.println("[BT] Client terputus");
  }

  // Kirim CustomerID saat Bluetooth terhubung (hanya sekali per koneksi)
  if (clientConnected && !customerIDSent) {
    SerialBT.println("CustomerID:" + customerID);
    Serial.println("[BT] Mengirim CustomerID: " + customerID);
    customerIDSent = true; // Set flag untuk mencegah pengiriman berulang
  }

  // Terima perintah via Bluetooth
  if (clientConnected && SerialBT.available()) {
    String cmd = SerialBT.readStringUntil('\n');
    cmd.trim(); // Hapus spasi atau newline
    if (cmd.length() > 0) { // Hanya proses jika ada perintah
        processBTCommand(cmd);
    }
  }

  // Kirim data FlowRate dan TotalVolume via Bluetooth secara berkala
  if (clientConnected && (now - prevSend >= 2000)) { // Kirim setiap 2 detik
    prevSend = now;
    String msg = "FlowRate:" + String(currentFlowRate, 2) + "/Lmin, Total:" + String(totalVolume, 3) + "L";
    SerialBT.println(msg);
    // Serial.println("[BT] Sent: " + msg); // Bisa di-uncomment untuk debugging di Serial Monitor USB
  }

  delay(10); // Delay kecil untuk stabilitas loop
}
