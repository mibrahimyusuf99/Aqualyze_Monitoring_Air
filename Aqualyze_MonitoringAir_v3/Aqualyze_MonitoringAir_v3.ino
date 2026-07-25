/*
 * ============================================================
 *  AQUALYZE - SISTEM MONITORING KUALITAS AIR KOLAM IKAN NILA
 * ============================================================
 *  Proyek     : Aqualyze
 *  versi      : 1.3
 *  Penyusun   : Muhammad Ibrahim Yusuf (NIM 10824006)
 *  Program    : D3 Teknik Komputer, UNIKOM
 *  Deskripsi:
 *    Firmware ESP32 untuk memonitor kualitas air kolam ikan nila
 *    secara real-time, meliputi suhu air, tingkat kekeruhan (NTU),
 *    dan pH air. Data dikirim ke broker MQTT (HiveMQ Cloud) dalam
 *    format JSON untuk ditampilkan di web dashboard, dan juga
 *    ditampilkan secara lokal di OLED SSD1306 0.96" (kuning+biru).
 *
 *  Hardware yang digunakan:
 *    - ESP32 DevKit (38-pin)
 *    - Sensor suhu waterproof DS18B20      -> GPIO 15
 *    - Sensor turbidity (kekeruhan) analog -> GPIO 32 (ADC1)
 *    - Sensor pH PH-4502C (masih dummy)    -> GPIO 33 (ADC1)
 *    - Buzzer aktif (indikator publish)    -> GPIO 25
 *    - OLED SSD1306 0.96" 128x64 I2C       -> SDA GPIO 21, SCL GPIO 22
 *                                            (VCC WAJIB ke 5V/VIN, bukan 3.3V)
 *
 *  Konektivitas:
 *    - WiFi (2.4GHz)
 *    - MQTT over TLS ke HiveMQ Cloud, port 8883
 *    - Sinkronisasi waktu via NTP (pool.ntp.org), zona waktu WIB (GMT+7)
 *    - Web dashboard lokal (port 80) -> buka http://<IP_ESP32>/ di browser
 *      untuk lihat data sensor real-time + log (tanpa perlu Serial Monitor/laptop,
 *      cocok dipakai saat device jalan pakai daya eksternal seperti baterai)
 *
 *  Alur pemrosesan data (WAJIB diikuti, jangan diubah urutannya):
 *    Sensor -> Raw Data -> Median Filter -> Threshold -> MQTT Publish
 *
 *    - Sampling sensor      : setiap 1 detik  (disimpan ke buffer, TIDAK langsung dikirim)
 *    - Median filter        : window 5 sampel, per sensor (suhu, pH, turbidity)
 *    - Threshold status     : dihitung dari HASIL MEDIAN (bukan raw data)
 *    - Publish MQTT         : setiap 1 menit (publish pertama langsung dikirim
 *                              begitu sample pertama tersedia, tidak menunggu
 *                              interval 1 menit penuh di awal)
 *
 *  Format payload  : JSON
 *  Interval publish: 1 menit (publish pertama secepatnya saat baru nyala)
 *  Topic MQTT      : monitoringair/data
 *
 *
 *  Status sensor:
 *    - Suhu (DS18B20)  : AKTIF (data asli dari hardware)
 *    - Turbidity       : AKTIF (data asli dari hardware, hasil rata-rata 10 sampel ADC)
 *    - pH              : DUMMY (sensor fisik belum terpasang)
 *    - Lokasi (GPS)    : DUMMY (lat/lon tetap di kampus UNIKOM, altitude sedikit fluktuatif)
 *
 *  Catatan kalibrasi:
 *    Konversi tegangan ke NTU (fungsi konversiNTU) menggunakan
 *    interpolasi linear antara titik kalibrasi air jernih dan air
 *    keruh (V_JERNIH, V_KERUH). Sesuaikan kedua nilai ini jika
 *    hasil pembacaan NTU tidak sesuai kondisi air sebenarnya.
 *
 *  Catatan threshold:
 *    Ambang batas Normal/Warning/Danger untuk tiap sensor adalah
 *    ESTIMASI berdasarkan referensi umum budidaya ikan nila.
 *    Sesuaikan konstanta SUHU_*, PH_*, dan NTU_* di bawah sesuai
 *    kebutuhan/standar yang dipakai di sistem kamu.
 *
 * ============================================================
 */

#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebServer.h>

// ==========================================
// DEKLARASI PIN ESP32 (38-PIN)
// ==========================================
const int PIN_SUHU = 15;       // GPIO 15 untuk Data DS18B20
const int PIN_TURBIDITY = 32; // GPIO 32 untuk Analog Turbidity (ADC1)
const int PIN_PH = 33;        // GPIO 33 untuk Analog pH PH-4502C (ADC1)
const int PIN_BUZZER = 25;    // GPIO 25 untuk Buzzer
// OLED pakai jalur I2C default ESP32: SDA -> GPIO 21, SCL -> GPIO 22

// Setup sensor suhu DS18B20
OneWire oneWire(PIN_SUHU);
DallasTemperature sensors(&oneWire);

// ==========================================
// KONFIGURASI OLED SSD1306 0.96" (KUNING+BIRU)
// ==========================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define OLED_ADDRESS  0x3C   // Kalau layar gak nyala, coba 0x3D

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledTersedia = false; // Supaya loop() gak error kalau OLED gagal init

// ==========================================
// KONFIGURASI WIFI
// ==========================================
const char* WIFI_SSID     = "ibrahim";
const char* WIFI_PASSWORD = "ibrahimy";

// ==========================================
// KONFIGURASI MQTT - HIVEMQ CLOUD
// ==========================================
const char* MQTT_HOST = "fcb0ad941e3f418f8dc1d16332c8fcb9.s1.eu.hivemq.cloud";
const int   MQTT_PORT = 8883; // Port TLS
const char* MQTT_USER = "aqualyze";
const char* MQTT_PASS = "aqualyze123";
const char* MQTT_CLIENT_ID = "esp32_aqualyze";
const char* MQTT_TOPIC = "monitoringair/data";

// ==========================================
// IDENTITAS DEVICE
// ==========================================
const char* DEVICE_ID = "Aqualyze-Nila-001"; // 
const char* NAMA_DEVICE = "Aqualyze Nila 1"; // 
const char* LOKASI_DEVICE = "Kolam Nila A";     
unsigned long messageCounter = 0; // Counter buat message_id, naik terus tiap publish

// ==========================================
// DATA LOKASI DUMMY (GPS) - KAMPUS UNIKOM BANDUNG
// ==========================================
// Device ini statis (gak gerak), jadi lat/lon tetap.
// Altitude dibikin sedikit berfluktuasi biar mirip noise GPS asli.
const float LOKASI_LATITUDE  = -6.8914;   // Dummy, lokasi kampus UNIKOM
const float LOKASI_LONGITUDE = 107.6107;  // Dummy, lokasi kampus UNIKOM
const float ALTITUDE_DASAR   = 750.0;     // Dummy, rata-rata ketinggian area Bandung (mdpl)

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

// ==========================================
// WEB SERVER (buat monitoring tanpa laptop/serial monitor)
// Akses lewat browser: http://<IP_ESP32>/
// ==========================================
WebServer server(80);
String logBuffer = "";              // "Serial Monitor" versi web
const int MAX_LOG_CHARS = 3000;     // Batasi ukuran biar RAM gak jebol
String payloadTerakhir = "";        // Payload JSON terakhir yang berhasil disusun

// ==========================================
// TIME BASE (NON-BLOCKING, PAKAI millis())
// ==========================================
unsigned long lastSample  = 0;
unsigned long lastPublish = 0;
const unsigned long SAMPLE_INTERVAL  = 1000;   // Sampling sensor setiap 1 detik
const unsigned long PUBLISH_INTERVAL = 60000;  // Publish MQTT setiap 1 menit
bool publishPertama = true; // Supaya publish pertama gak nunggu 1 menit penuh

// ==========================================
// KONFIGURASI MEDIAN FILTER
// ==========================================
const int MEDIAN_WINDOW = 5; // Jumlah sampel per window filter

float bufSuhu[MEDIAN_WINDOW];
float bufNTU[MEDIAN_WINDOW];
float bufPH[MEDIAN_WINDOW];
int   bufIndex     = 0; // Posisi tulis berikutnya di buffer (circular)
int   jumlahSampel = 0; // Jumlah sampel valid yang sudah terkumpul (maks MEDIAN_WINDOW)

// Hasil median terakhir (dipakai untuk threshold & payload MQTT)
float suhuFiltered = 0.0;
float ntuFiltered  = 0.0;
float phFiltered   = 0.0;

// Tegangan turbidity mentah terakhir (disimpan buat referensi di payload,
// TIDAK ikut di-median-filter karena bukan bagian dari alur threshold)
float teganganTurbidityTerakhir = 0.0;

// ==========================================
// KONFIGURASI THRESHOLD (NORMAL / WARNING / DANGER)
// ==========================================
// CATATAN: nilai di bawah ini estimasi umum untuk kolam ikan nila,
// sesuaikan dengan standar yang dipakai sistem kamu kalau perlu.

// Suhu (Celsius)
const float SUHU_NORMAL_MIN  = 25.0;
const float SUHU_NORMAL_MAX  = 32.0;
const float SUHU_WARNING_MIN = 20.0;
const float SUHU_WARNING_MAX = 35.0;

// pH
const float PH_NORMAL_MIN  = 6.5;
const float PH_NORMAL_MAX  = 8.5;
const float PH_WARNING_MIN = 5.5;
const float PH_WARNING_MAX = 9.5;

// Turbidity (NTU) - Normal <=10 sesuai logika dashboard yang dipakai
const float NTU_NORMAL_MAX   = 10.0;
const float NTU_WARNING_MAX  = 25.0;

// Status hasil threshold (pointer ke string literal, tidak alokasi heap)
const char* statusSuhu      = "Normal";
const char* statusPH        = "Normal";
const char* statusTurbidity = "Normal";

// ==========================================
// KONFIGURASI NTP (WAKTU)
// ==========================================
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 7 * 3600; // WIB = GMT+7
const int   DAYLIGHT_OFFSET_SEC = 0;

// ==========================================
// FUNGSI LOG: TULIS KE SERIAL + SIMPAN KE BUFFER WEB
// ==========================================
// Dipakai untuk pesan-pesan penting (koneksi, hasil sensor, publish),
void logPesan(String pesan) {
  Serial.println(pesan);
  logBuffer += pesan;
  logBuffer += "\n";
  if (logBuffer.length() > MAX_LOG_CHARS) {
    logBuffer.remove(0, logBuffer.length() - MAX_LOG_CHARS);
  }
}

// ==========================================
// FUNGSI BUZZER: BEEP N KALI
// ==========================================
// Asumsi pakai buzzer AKTIF (tinggal HIGH/LOW, bukan passive buzzer
// yang butuh tone() dengan frekuensi tertentu).
void beepBuzzer(int jumlahBeep) {
  for (int i = 0; i < jumlahBeep; i++) {
    digitalWrite(PIN_BUZZER, HIGH);
    delay(100);
    digitalWrite(PIN_BUZZER, LOW);
    delay(100);
  }
}

// ==========================================
// FUNGSI TAMPILAN OLED
// Baris atas (kuning, y:0-15)  = judul/header
// Baris bawah (biru, y:16-63) = data sensor + status
// ==========================================
void tampilkanOLED(float suhu, float ntu, float ph, bool statusMQTT) {
  if (!oledTersedia) return;

  display.clearDisplay();

  // --- Area kuning: judul ---
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("AQUALYZE - KOLAM NILA");

  // --- Area biru: data sensor (hasil median filter) ---
  display.setCursor(0, 20);
  display.print("Suhu   : ");
  display.print(suhu, 1);
  display.println(" C");

  display.setCursor(0, 32);
  display.print("Turbid : ");
  display.print(ntu, 0);
  display.println(" NTU");

  display.setCursor(0, 44);
  display.print("pH     : ");
  display.println(ph, 2);

  display.setCursor(0, 56);
  display.print("MQTT   : ");
  display.println(statusMQTT ? "Publish OK" : "Gagal");

  display.display();
}

// Tampilan status singkat (dipakai saat proses WiFi/MQTT connect)
void tampilkanStatusOLED(const char* baris1, const char* baris2) {
  if (!oledTersedia) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("AQUALYZE");
  display.setCursor(0, 20);
  display.println(baris1);
  display.setCursor(0, 32);
  display.println(baris2);
  display.display();
}

// ==========================================
// FUNGSI KONVERSI TEGANGAN TURBIDITY -> NTU
// ==========================================
float konversiNTU(float v) {
    const float V_JERNIH = 1.638;
    const float V_KERUH  = 1.162;

    if (v >= V_JERNIH)
        return 5.0;

    if (v <= V_KERUH)
        return 150.0;

    // Interpolasi linear
    float ntu = 5.0 +
                (V_JERNIH - v) *
                (150.0 - 5.0) /
                (V_JERNIH - V_KERUH);

    return round(ntu);
}

// ==========================================
// FUNGSI MEDIAN (INSERTION SORT, COCOK UNTUK N KECIL)
// ==========================================
float hitungMedian(float arr[], int n) {
  float temp[MEDIAN_WINDOW];
  for (int i = 0; i < n; i++) temp[i] = arr[i];

  // Insertion sort menaik - efisien untuk array sekecil ini (n<=5)
  for (int i = 1; i < n; i++) {
    float kunci = temp[i];
    int j = i - 1;
    while (j >= 0 && temp[j] > kunci) {
      temp[j + 1] = temp[j];
      j--;
    }
    temp[j + 1] = kunci;
  }

  if (n % 2 == 1) {
    return temp[n / 2];
  } else {
    return (temp[n / 2 - 1] + temp[n / 2]) / 2.0;
  }
}

// Fungsi sinkronisasi waktu ke server NTP
void syncWaktu() {
  Serial.print("Sinkronisasi waktu NTP");
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  struct tm timeinfo;
  int percobaan = 0;
  while (!getLocalTime(&timeinfo) && percobaan < 20) {
    Serial.print(".");
    delay(500);
    percobaan++;
  }

  if (percobaan >= 20) {
    Serial.println(" Gagal sinkronisasi waktu, cek koneksi internet.");
  } else {
    Serial.println(" Berhasil!");
  }
}

// ==========================================
// FUNGSI KONEKSI WIFI
// ==========================================
void setupWifi() {
  logPesan("Menghubungkan ke WiFi: " + String(WIFI_SSID));
  tampilkanStatusOLED("Menghubungkan WiFi", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  logPesan("WiFi Terhubung. IP Address: " + WiFi.localIP().toString());
  tampilkanStatusOLED("WiFi Terhubung", WiFi.localIP().toString().c_str());
}

// ==========================================
// FUNGSI KONEKSI MQTT (dengan reconnect otomatis + anti-stuck)
// ==========================================
void reconnectMQTT() {
  int percobaanGagal = 0;

  while (!mqttClient.connected()) {
    logPesan("Menghubungkan ke MQTT Broker...");
    tampilkanStatusOLED("Menghubungkan", "MQTT Broker...");

    espClient.stop();

    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
      logPesan("MQTT Berhasil terhubung! Free heap: " + String(ESP.getFreeHeap()) + " bytes");
      percobaanGagal = 0;
      tampilkanStatusOLED("MQTT Terhubung", "Siap publish data");
    
      delay(500);
    } else {
      percobaanGagal++;
      logPesan("MQTT Gagal, rc=" + String(mqttClient.state()) + " (percobaan ke-" + String(percobaanGagal) + ") -> coba lagi dalam 3 detik");
      tampilkanStatusOLED("MQTT Gagal", "Coba lagi...");

      if (percobaanGagal >= 10) {
        logPesan("Terlalu banyak kegagalan MQTT, restart ESP32...");
        tampilkanStatusOLED("Gagal Total", "Restarting...");
        delay(1000);
        ESP.restart();
      }

      delay(3000);
    }
  }
}

// ==========================================
// 1. BACA SENSOR (RAW DATA) -> MASUKKAN KE BUFFER
// ==========================================
void readSensor() {
  // ---- Suhu (DS18B20) ----
  static float suhuTerakhirValid = 27.0; // fallback awal kalau sensor belum pernah valid

  sensors.requestTemperatures();
  float suhuRaw = sensors.getTempCByIndex(0);

  if (suhuRaw == DEVICE_DISCONNECTED_C || suhuRaw == 85.0 || suhuRaw < -40 || suhuRaw > 125) {
    Serial.println("Pembacaan suhu gagal, pakai nilai terakhir yang valid.");
    suhuRaw = suhuTerakhirValid;
  } else {
    suhuTerakhirValid = suhuRaw;
  }

  // ---- Turbidity ----
  long totalADC = 0;
  for (int i = 0; i < 10; i++) {
    totalADC += analogRead(PIN_TURBIDITY);
    delay(5); // pembacaan ADC lokal, bukan bagian dari time base utama
  }
  int adcTurbidity = totalADC / 10;

  // Jika sensor disupply 3.3V
  float teganganTurbidity = (adcTurbidity * 3.3) / 4095.0;
  // Jika nanti sensor disupply 5V + pembagi 1:2, kalikan 1.5:
  // teganganTurbidity *= 1.5;

  teganganTurbidityTerakhir = teganganTurbidity; // disimpan buat referensi payload
  float ntuRaw = konversiNTU(teganganTurbidity);

  // ---- pH (dummy, sensor fisik belum terpasang) ----
  float phRaw = random(680, 750) / 100.0;

  // ---- Simpan raw data ke buffer median filter ----
  bufSuhu[bufIndex] = suhuRaw;
  bufNTU[bufIndex]  = ntuRaw;
  bufPH[bufIndex]   = phRaw;

  bufIndex = (bufIndex + 1) % MEDIAN_WINDOW;
  if (jumlahSampel < MEDIAN_WINDOW) jumlahSampel++;

  logPesan("[Sample] Suhu: " + String(suhuRaw, 2) + " C | Tegangan Turbidity: " + String(teganganTurbidity, 3) + " V | NTU: " + String(ntuRaw, 0) + " | pH: " + String(phRaw, 2));
}

// ==========================================
// 2. MEDIAN FILTER -> UPDATE NILAI TERFILTER
// ==========================================
void medianFilter() {
  if (jumlahSampel == 0) return; // belum ada data sama sekali

  suhuFiltered = hitungMedian(bufSuhu, jumlahSampel);
  ntuFiltered  = hitungMedian(bufNTU, jumlahSampel);
  phFiltered   = hitungMedian(bufPH, jumlahSampel);
}

// ==========================================
// 3. THRESHOLD -> STATUS NORMAL / WARNING / DANGER
//    (dihitung dari HASIL MEDIAN, bukan raw data)
// ==========================================
void calculateThreshold() {
  // --- Status Suhu ---
  if (suhuFiltered >= SUHU_NORMAL_MIN && suhuFiltered <= SUHU_NORMAL_MAX) {
    statusSuhu = "Normal";
  } else if (suhuFiltered >= SUHU_WARNING_MIN && suhuFiltered <= SUHU_WARNING_MAX) {
    statusSuhu = "Warning";
  } else {
    statusSuhu = "Danger";
  }

  // --- Status pH ---
  if (phFiltered >= PH_NORMAL_MIN && phFiltered <= PH_NORMAL_MAX) {
    statusPH = "Normal";
  } else if (phFiltered >= PH_WARNING_MIN && phFiltered <= PH_WARNING_MAX) {
    statusPH = "Warning";
  } else {
    statusPH = "Danger";
  }

  // --- Status Turbidity ---
  if (ntuFiltered <= NTU_NORMAL_MAX) {
    statusTurbidity = "Normal";
  } else if (ntuFiltered <= NTU_WARNING_MAX) {
    statusTurbidity = "Warning";
  } else {
    statusTurbidity = "Danger";
  }
}

// ==========================================
// 4. PUBLISH MQTT (pakai hasil filter + status terakhir)
// ==========================================
void publishMQTT() {
  // ---- Ambil waktu saat ini (timestamp ISO 8601) ----
  struct tm timeinfo;
  char strTimestampISO[26]; // format: 2026-07-04T14:10:33+07:00

  if (getLocalTime(&timeinfo)) {
    char strISOBase[20];
    strftime(strISOBase, sizeof(strISOBase), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    snprintf(strTimestampISO, sizeof(strTimestampISO), "%s+07:00", strISOBase);
  } else {
    strcpy(strTimestampISO, "0000-00-00T00:00:00+07:00");
    Serial.println("Peringatan: waktu belum tersinkron, mencoba sinkron ulang...");
    syncWaktu();
  }

  // ---- Data lokasi dummy (GPS) - kampus UNIKOM ----
  float dummyLatitude  = LOKASI_LATITUDE;
  float dummyLongitude = LOKASI_LONGITUDE;
  float dummyAltitude  = ALTITUDE_DASAR + (random(-300, 300) / 100.0);

  // ---- Susun message_id ----
  messageCounter++;
  char strMessageId[40];
  snprintf(strMessageId, sizeof(strMessageId), "MSG-%s-%010lu", DEVICE_ID, messageCounter);

  // ---- Susun payload JSON ----
  JsonDocument doc; // ArduinoJson v7 (kalau pakai v6, ganti jadi StaticJsonDocument<700> doc;)

  doc["message_id"] = strMessageId;
  doc["device_id"] = DEVICE_ID; // dipakai buat verifikasi data device ini masuk atau tidak
  doc["lokasi"] = LOKASI_DEVICE;
  doc["timestamp"] = strTimestampISO;

  JsonObject status = doc["status"].to<JsonObject>();
  status["node_status"] = "online";
  status["ip"] = WiFi.localIP().toString();

  // Data yang dikirim adalah HASIL MEDIAN FILTER, lengkap dengan status threshold-nya
  JsonObject data = doc["data"].to<JsonObject>();
  data["suhu"] = round(suhuFiltered * 100) / 100.0;
  data["status_suhu"] = statusSuhu;
  data["turbidity_v"] = round(teganganTurbidityTerakhir * 100) / 100.0;
  data["turbidity_ntu"] = round(ntuFiltered * 100) / 100.0;
  data["status_kekeruhan"] = statusTurbidity;
  data["ph"] = round(phFiltered * 100) / 100.0;
  data["status_ph"] = statusPH;

  JsonObject lokasi = doc["location"].to<JsonObject>();
  lokasi["latitude"] = round(dummyLatitude * 1000000) / 1000000.0;
  lokasi["longitude"] = round(dummyLongitude * 1000000) / 1000000.0;
  lokasi["altitude_mdpl"] = round(dummyAltitude * 100) / 100.0;

  char jsonBuffer[700];
  serializeJson(doc, jsonBuffer);
  payloadTerakhir = String(jsonBuffer); // disimpan buat ditampilkan di web

  logPesan("Ukuran payload: " + String(strlen(jsonBuffer)) + " byte");

  bool suksesPublish = mqttClient.publish(MQTT_TOPIC, jsonBuffer);

  if (suksesPublish) {
    logPesan("Publish BERHASIL ke topic '" + String(MQTT_TOPIC) + "': " + payloadTerakhir);
    beepBuzzer(3); // Beep 3 kali tanda data berhasil terkirim
  } else {
    logPesan("Publish GAGAL! state code: " + String(mqttClient.state()));
  }

  tampilkanOLED(suhuFiltered, ntuFiltered, phFiltered, suksesPublish);
}

// ==========================================
// 5. TIME BASE SCHEDULER (NON-BLOCKING)
// ==========================================
void updateTimeBase() {
  unsigned long now = millis();

  // --- Sampling sensor setiap 1 detik: Raw Data -> Median Filter -> Threshold ---
  if (now - lastSample >= SAMPLE_INTERVAL) {
    lastSample = now;
    readSensor();
    medianFilter();
    calculateThreshold();

    // Publish SECEPATNYA begitu sample pertama tersedia (saat device baru nyala),
    if (publishPertama && jumlahSampel > 0) {
      lastPublish = now;
      publishPertama = false;
      publishMQTT();
    }
  }

  // --- Publish MQTT setiap 1 menit (di luar publish pertama), pakai hasil filter+threshold terakhir ---
  if (!publishPertama && now - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = now;
    publishMQTT();
  }
}

// ==========================================
// WEB SERVER: HALAMAN DASHBOARD
// ==========================================
// Kembalikan nama class CSS sesuai status (buat pewarnaan di halaman web)
String kelasStatus(const char* status) {
  if (strcmp(status, "Normal") == 0) return "normal";
  if (strcmp(status, "Warning") == 0) return "warning";
  return "danger";
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='5'>"; // auto-refresh tiap 5 detik
  html += "<title>Aqualyze Monitor</title>";
  html += "<style>";
  html += "body{font-family:sans-serif;background:#0f172a;color:#e2e8f0;padding:20px;max-width:700px;margin:auto;}";
  html += "h1{color:#38bdf8;font-size:22px;} h2{font-size:16px;color:#94a3b8;margin-bottom:8px;}";
  html += ".card{background:#1e293b;border-radius:10px;padding:16px;margin-bottom:16px;}";
  html += ".normal{color:#4ade80;font-weight:bold;} .warning{color:#facc15;font-weight:bold;} .danger{color:#f87171;font-weight:bold;}";
  html += "pre{background:#020617;padding:12px;border-radius:8px;max-height:260px;overflow-y:auto;white-space:pre-wrap;font-size:12px;line-height:1.5;}";
  html += "</style></head><body>";

  html += "<h1>Aqualyze - " + String(DEVICE_ID) + "</h1>";

  html += "<div class='card'><h2>Status Device</h2>";
  html += "Device ID: " + String(DEVICE_ID) + "<br>";
  html += "Lokasi: " + String(LOKASI_DEVICE) + "<br>";
  html += "IP Address: " + WiFi.localIP().toString() + "<br>";
  html += "Status MQTT: " + String(mqttClient.connected() ? "Terhubung" : "Terputus") + "<br>";
  html += "Uptime: " + String(millis() / 1000) + " detik<br>";
  html += "Free Heap: " + String(ESP.getFreeHeap()) + " bytes<br>";
  html += "Total Data Terkirim: " + String(messageCounter) + "</div>";

  html += "<div class='card'><h2>Data Sensor (Hasil Median Filter)</h2>";
  html += "Suhu: " + String(suhuFiltered, 2) + " &deg;C - <span class='" + kelasStatus(statusSuhu) + "'>" + String(statusSuhu) + "</span><br>";
  html += "Turbidity: " + String(ntuFiltered, 0) + " NTU (" + String(teganganTurbidityTerakhir, 3) + " V) - <span class='" + kelasStatus(statusTurbidity) + "'>" + String(statusTurbidity) + "</span><br>";
  html += "pH: " + String(phFiltered, 2) + " - <span class='" + kelasStatus(statusPH) + "'>" + String(statusPH) + "</span></div>";

  html += "<div class='card'><h2>Payload MQTT Terakhir</h2><pre>" + payloadTerakhir + "</pre></div>";

  html += "<div class='card'><h2>Log (mirip Serial Monitor)</h2><pre>" + logBuffer + "</pre></div>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}

void setupWebServer() {
  server.on("/", handleRoot);
  server.begin();
  logPesan("Web server aktif. Buka http://" + WiFi.localIP().toString() + "/ di browser.");
}

void setup() {
  Serial.begin(115200);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  // --- Inisialisasi OLED (SDA=21, SCL=22) ---
  // Catatan: modul OLED ini butuh VCC 5V (bukan 3.3V) supaya bisa render.
  // Clock I2C diturunkan ke 100kHz karena modul ini kurang stabil di 400kHz (default).
  Wire.begin(21, 22);
  Wire.setClock(100000);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED SSD1306 tidak terdeteksi!");
    oledTersedia = false;
  } else {
    oledTersedia = true;
    display.dim(false); // Pastikan tidak dalam mode redup (dim)
    display.ssd1306_command(SSD1306_SETCONTRAST);
    display.ssd1306_command(0xFF); // Contrast maksimal (0-255)
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("AQUALYZE");
    display.setCursor(0, 20);
    display.println("Inisialisasi...");
    display.display();
  }

  sensors.begin();

  // Resolusi maksimal
  sensors.setResolution(12);

  // Tunggu sampai konversi selesai
  sensors.setWaitForConversion(true);

  // Konversi pertama
  sensors.requestTemperatures();
  delay(800);

  Serial.println();
  Serial.println("===== CEK DS18B20 =====");
  Serial.print("GPIO Sensor : ");
  Serial.println(PIN_SUHU);
  Serial.print("Jumlah Sensor : ");
  Serial.println(sensors.getDeviceCount());

  DeviceAddress sensorAddress;
  if (sensors.getAddress(sensorAddress, 0)) {
    Serial.println("DS18B20 TERDETEKSI");
    Serial.print("ROM Address : ");
    for (uint8_t i = 0; i < 8; i++) {
      if (sensorAddress[i] < 16) Serial.print("0");
      Serial.print(sensorAddress[i], HEX);
      Serial.print(" ");
    }
    Serial.println();
  } else {
    Serial.println("DS18B20 TIDAK DITEMUKAN!");
  }

  randomSeed(esp_random()); // Seed random biar nilai dummy (pH) lebih variatif

  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW); // Pastikan buzzer mati di awal

  setupWifi();
  setupWebServer();
  syncWaktu();
  espClient.setInsecure();
  espClient.setTimeout(20000);      // timeout socket TLS, dalam milidetik
  mqttClient.setSocketTimeout(20);  // timeout socket MQTT, dalam detik
  mqttClient.setKeepAlive(60);      // ping tiap 60 detik,
  mqttClient.setBufferSize(768);
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);

  // Inisialisasi time base
  lastSample = millis();
  lastPublish = millis();

  Serial.println("--- Aqualyze Siap: Sampling 1s, Median Filter, Threshold, Publish 1 menit, Web Dashboard aktif ---");
}

void loop() {
  server.handleClient(); // Layani permintaan browser ke dashboard web

  if (WiFi.status() != WL_CONNECTED) {
    setupWifi();
  }
  if (!mqttClient.connected()) {
    Serial.print("MQTT terputus, state code: ");
    Serial.println(mqttClient.state());
    reconnectMQTT();
  }
  mqttClient.loop();

  updateTimeBase();
}
