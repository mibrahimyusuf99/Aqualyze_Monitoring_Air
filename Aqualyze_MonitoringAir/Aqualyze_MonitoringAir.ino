/*
 * ============================================================
 *  AQUALYZE - SISTEM MONITORING KUALITAS AIR KOLAM IKAN NILA
 * ============================================================
 *
 *  Deskripsi:
 *    Firmware ESP32 untuk memonitor kualitas air kolam ikan nila
 *    secara real-time, meliputi suhu air, tingkat kekeruhan (NTU),
 *    dan pH air. Data dikirim ke broker MQTT (HiveMQ Cloud) dalam
 *    format JSON untuk ditampilkan di web dashboard.
 *
 *  Hardware yang digunakan:
 *    - ESP32 DevKit (38-pin)
 *    - Sensor suhu waterproof DS18B20      -> GPIO 15
 *    - Sensor turbidity (kekeruhan) analog -> GPIO 32 (ADC1)
 *    - Sensor pH PH-4502C (masih dummy)    -> GPIO 33 (ADC1)
 *    - Buzzer aktif (indikator publish)    -> GPIO 25
 *
 *  Konektivitas:
 *    - WiFi (2.4GHz)
 *    - MQTT over TLS ke HiveMQ Cloud, port 8883
 *    - Sinkronisasi waktu via NTP (pool.ntp.org), zona waktu WIB (GMT+7)
 *
 *  Format payload  : JSON
 *  Interval publish: 60 detik
 *  Topic MQTT      : monitoringair/data
 *
 *  Status sensor:
 *    - Suhu (DS18B20)  : AKTIF (data asli dari hardware)
 *    - Turbidity       : AKTIF (data asli dari hardware, hasil rata-rata 10 sampel ADC)
 *    - pH              : DUMMY (sensor fisik belum terpasang)
 *
 *  Catatan kalibrasi:
 *    Konversi tegangan ke NTU (fungsi konversiNTU) menggunakan
 *    interpolasi linear antara titik kalibrasi air jernih dan air
 *    keruh (V_JERNIH, V_KERUH). Sesuaikan kedua nilai ini jika
 *    hasil pembacaan NTU tidak sesuai kondisi air sebenarnya.
 *
 *  Proyek     : Capstone - Aqualyze
 *  Penyusun   : Muhammad Ibrahim Yusuf (NIM 10824006)
 *  Program    : D3 Teknik Komputer, UNIKOM
 * ============================================================
 */

#include <OneWire.h>
#include <DallasTemperature.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>

// ==========================================
// DEKLARASI PIN ESP32 (38-PIN)
// ==========================================
const int PIN_SUHU = 15;       // GPIO 15 untuk Data DS18B20
const int PIN_TURBIDITY = 32; // GPIO 32 untuk Analog Turbidity (ADC1)
const int PIN_PH = 33;        // GPIO 33 untuk Analog pH PH-4502C (ADC1)
const int PIN_BUZZER = 25;    // GPIO 25 untuk Buzzer 

// Setup sensor suhu DS18B20
OneWire oneWire(PIN_SUHU);
DallasTemperature sensors(&oneWire);

// ==========================================
// KONFIGURASI WIFI
// ==========================================
const char* WIFI_SSID     = ".";
const char* WIFI_PASSWORD = "ibrahimy";

// ==========================================
// KONFIGURASI MQTT - HIVEMQ CLOUD
// ==========================================
const char* MQTT_HOST = "fcb0ad941e3f418f8dc1d16332c8fcb9.s1.eu.hivemq.cloud";
const int   MQTT_PORT = 8883;// Port TLS
const char* MQTT_USER = "aqualyze";
const char* MQTT_PASS = "aqualyze123";
const char* MQTT_CLIENT_ID = "esp32_aqualyze";
const char* MQTT_TOPIC = "monitoringair/data";

// ==========================================
// IDENTITAS DEVICE
// ==========================================
const char* DEVICE_ID = "Aqualyze-monitor"; // Ganti sesuai nama node/device kamu
unsigned long messageCounter = 0; // Counter buat message_id, naik terus tiap publish

WiFiClientSecure espClient;
PubSubClient mqttClient(espClient);

unsigned long lastPublish = 0;
const unsigned long PUBLISH_INTERVAL = 60000; // 60 detik sesuai permintaan

// ==========================================
// KONFIGURASI NTP (WAKTU)
// ==========================================
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 7 * 3600; // WIB = GMT+7
const int   DAYLIGHT_OFFSET_SEC = 0;

// ==========================================
// FUNGSI BUZZER: BEEP 3 KALI
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
// FUNGSI KONVERSI TEGANGAN TURBIDITY -> NTU
// ==========================================

float konversiNTU(float v)
{
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
void connectWiFi() {
  Serial.print("Menghubungkan ke WiFi: ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi Terhubung. IP Address: ");
  Serial.println(WiFi.localIP());
}

// ==========================================
// FUNGSI KONEKSI MQTT (dengan reconnect otomatis + anti-stuck)
// ==========================================
void connectMQTT() {
  int percobaanGagal = 0;

  while (!mqttClient.connected()) {
    Serial.print("Menghubungkan ke MQTT Broker...");

    // Pastikan socket lama benar-benar ditutup sebelum coba lagi,
    // supaya tidak ada koneksi TLS "menggantung" yang bikin connect() hang.
    espClient.stop();

    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
      Serial.println(" Berhasil!");
      Serial.print("   Free heap: ");
      Serial.print(ESP.getFreeHeap());
      Serial.println(" bytes");
      percobaanGagal = 0;

      // Jeda singkat biar TLS session stabil dulu sebelum dipakai,
      // dan biar gak langsung "hammer" broker kalau ternyata langsung putus lagi.
      delay(500);
    } else {
      percobaanGagal++;
      Serial.print(" Gagal, rc=");
      Serial.print(mqttClient.state());
      Serial.print(" (percobaan ke-");
      Serial.print(percobaanGagal);
      Serial.println(") -> coba lagi dalam 3 detik");

      // Kalau gagal terus-menerus (misal TLS handshake macet total),
      // restart ESP32 daripada nge-hang selamanya.
      if (percobaanGagal >= 10) {
        Serial.println("Terlalu banyak kegagalan MQTT, restart ESP32...");
        delay(1000);
        ESP.restart();
      }

      delay(3000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

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

  connectWiFi();
  syncWaktu();

  // Untuk testing cepat, sertifikat TLS di-skip verifikasinya.
  // Untuk produksi/keamanan lebih baik, ganti dengan setCACert() pakai root CA HiveMQ.
  espClient.setInsecure();

  // PENTING: batasi timeout supaya koneksi TLS/MQTT tidak hang tanpa batas
  // waktu, tapi jangan terlalu ketat karena jaringan hotspot/mobile data
  // biasanya lebih lambat & gak stabil dibanding WiFi rumahan/kampus.
  espClient.setTimeout(20000);      // timeout socket TLS, dalam milidetik
  mqttClient.setSocketTimeout(20);  // timeout socket MQTT, dalam detik
  mqttClient.setKeepAlive(60);      // ping tiap 60 detik, lebih toleran untuk jaringan lambat

  // PENTING: buffer default PubSubClient cuma 256 byte, sedangkan payload
  // JSON kita bisa lebih dari itu (topic + payload digabung). Kalau kelewat,
  // publish() akan gagal diam-diam tanpa error yang jelas. Naikkan ke 512.
  mqttClient.setBufferSize(512);

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);

  Serial.println("--- Sistem Monitoring Air (SUHU & TURBIDITY ASLI, pH DUMMY + MQTT JSON) Siap ---");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }
  if (!mqttClient.connected()) {
    Serial.print("MQTT terputus, state code: ");
    Serial.println(mqttClient.state());
    connectMQTT();
  }
  mqttClient.loop();

  unsigned long now = millis();
  if (now - lastPublish >= PUBLISH_INTERVAL) {
    lastPublish = now;

    // ==========================================
    // 1. SENSOR SUHU DS18B20
    // ==========================================
    float suhuC = 0.0;

    /*
    // --- PROGRAM DUMMY SENSOR SUHU (DIMATIKAN / KOMENTAR) ---
    suhuC = random(2400, 3000) / 100.0;
    */

    // --- PROGRAM ASLI SENSOR SUHU DS18B20 (AKTIF) ---
    //--------------------------------------------------
    // BACA SENSOR SUHU DS18B20
    //--------------------------------------------------

    sensors.requestTemperatures();
    float suhu = sensors.getTempCByIndex(0);

    if (suhu == DEVICE_DISCONNECTED_C ||
        suhu == 85.0 ||
        suhu < -40 ||
        suhu > 125)
    {
        Serial.println("Pembacaan suhu gagal!");

        // jangan kirim 0
        // pakai nilai terakhir yang valid
    }
    else
    {
        suhuC = suhu;

        Serial.print("Suhu Air : ");
        Serial.print(suhuC,2);
        Serial.println(" C");
    }

    // ==========================================
    // 2. SENSOR TURBIDITY
    // ==========================================
    
    /*
    // --- PROGRAM DUMMY SENSOR TURBIDITY (DIMATIKAN / KOMENTAR) ---
    teganganSensorTurbidity = random(350, 420) / 100.0;
    */

    // --- PROGRAM ASLI SENSOR TURBIDITY (AKTIF) ---
    // ==========================================
    // 2. SENSOR TURBIDITY
    // ==========================================

    long totalADC = 0;

    for (int i = 0; i < 10; i++) {
      totalADC += analogRead(PIN_TURBIDITY);
      delay(5);
    }

    int adcTurbidity = totalADC / 10;

    // Jika sensor disupply 3.3V
    float teganganSensorTurbidity =
    (adcTurbidity * 3.3) / 4095.0;

    // Jika nanti sensor disupply 5V + pembagi 1:2,
    // gunakan:
    // teganganSensorTurbidity *= 1.5;

    float nilaiNTU = konversiNTU(teganganSensorTurbidity);

    Serial.print("ADC Turbidity : ");
    Serial.println(adcTurbidity);

    Serial.print("Tegangan : ");
    Serial.print(teganganSensorTurbidity,3);
    Serial.println(" V");

    Serial.print("NTU : ");
    Serial.println(nilaiNTU);
    
    // --- PROGRAM DUMMY NTU (DIMATIKAN / KOMENTAR) ---
    // Sebelumnya dipakai buat simulasi sebelum sensor fisik terpasang.
    // float nilaiNTU;
    // int peluangKeruh = random(0, 100);
    // if (peluangKeruh < 85) {
    //   nilaiNTU = random(0, 800) / 100.0;
    // } else {
    //   nilaiNTU = random(1000, 2500) / 100.0;
    // }
    // */


    // ==========================================
    // 3. SENSOR pH (MODEL PH-4502C)
    // ==========================================
    float nilaiPH = 0.0;

    // --- PROGRAM DUMMY SENSOR pH (AKTIF, BELUM ADA SENSOR FISIK) ---
    nilaiPH = random(680, 750) / 100.0;

    /*
    // --- PROGRAM ASLI SENSOR pH (DIMATIKAN / KOMENTAR) ---
    int adcpH = analogRead(PIN_PH);
    float teganganAdcpH = (adcpH * 3.3) / 4095.0;
    float teganganSensorPH = teganganAdcpH * 1.5;
    nilaiPH = 7.0 + ((2.50 - teganganSensorPH) / 0.18);
    */


    // ==========================================
    // 4. AMBIL WAKTU SAAT INI (TIMESTAMP ISO 8601)
    // ==========================================
    struct tm timeinfo;
    char strTimestampISO[26]; // format: 2026-07-04T14:10:33+07:00

    if (getLocalTime(&timeinfo)) {
      char strISOBase[20];
      strftime(strISOBase, sizeof(strISOBase), "%Y-%m-%dT%H:%M:%S", &timeinfo);
      snprintf(strTimestampISO, sizeof(strTimestampISO), "%s+07:00", strISOBase);
    } else {
      // Kalau waktu belum/gagal sinkron, tetap kirim data dengan nilai default
      strcpy(strTimestampISO, "0000-00-00T00:00:00+07:00");
      Serial.println("Peringatan: waktu belum tersinkron, mencoba sinkron ulang...");
      syncWaktu();
    }

    // ==========================================
    // 5. SUSUN PAYLOAD JSON
    // ==========================================
    messageCounter++;
    char strMessageId[40];
    snprintf(strMessageId, sizeof(strMessageId), "MSG-%s-%010lu", DEVICE_ID, messageCounter);

    JsonDocument doc; // ArduinoJson v7 (kalau pakai v6, ganti jadi StaticJsonDocument<400> doc;)

    doc["message_id"] = strMessageId;
    doc["device_id"] = DEVICE_ID;
    doc["timestamp"] = strTimestampISO;

    JsonObject status = doc["status"].to<JsonObject>();
    status["node_status"] = "online";
    status["ip"] = WiFi.localIP().toString();

    JsonObject data = doc["data"].to<JsonObject>();
    data["suhu"] = round(suhuC * 100) / 100.0;
    data["turbidity_v"] = round(teganganSensorTurbidity * 100) / 100.0;
    data["turbidity_ntu"] = round(nilaiNTU * 100) / 100.0;
    data["ph"] = round(nilaiPH * 100) / 100.0;

    char jsonBuffer[400];
    serializeJson(doc, jsonBuffer);

    // ==========================================
    // 6. PUBLISH KE MQTT
    // ==========================================
    Serial.print("Ukuran payload: ");
    Serial.print(strlen(jsonBuffer));
    Serial.println(" byte");

    bool suksesPublish = mqttClient.publish(MQTT_TOPIC, jsonBuffer);

    if (suksesPublish) {
      Serial.print("Publish BERHASIL ke topic '");
      Serial.print(MQTT_TOPIC);
      Serial.print("': ");
      Serial.println(jsonBuffer);

      beepBuzzer(3); // Beep 3 kali tanda data berhasil terkirim
    } else {
      Serial.print("Publish GAGAL! state code: ");
      Serial.println(mqttClient.state());
    }
  }
}
