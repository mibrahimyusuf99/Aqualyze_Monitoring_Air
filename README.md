# Aqualyze

Sistem monitoring kualitas air real-time untuk kolam ikan nila berbasis ESP32. Aqualyze membaca suhu, kekeruhan (turbidity/NTU), dan pH air, memfilter data dengan median filter, menentukan status kualitas air (Normal/Warning/Danger), lalu mengirimkan datanya dalam format JSON ke broker MQTT (HiveMQ Cloud) untuk ditampilkan di web dashboard.

## Fitur

- Pembacaan suhu air real-time menggunakan sensor **DS18B20**
- Pembacaan tingkat kekeruhan air (**NTU**) menggunakan sensor turbidity analog
- **Median filter** (window 5 sampel) untuk menyaring noise sebelum data diproses lebih lanjut
- **Threshold otomatis** — tiap sensor diklasifikasikan sebagai `Normal`, `Warning`, atau `Danger` berdasarkan hasil median
- Pengiriman data via **MQTT over TLS** ke HiveMQ Cloud
- Payload terstruktur dalam format **JSON**, lengkap dengan `message_id`, `device_id`, `timestamp` (ISO 8601), status per sensor, dan status koneksi
- Sinkronisasi waktu otomatis melalui **NTP** (WIB, GMT+7)
- **Web dashboard bawaan ESP32** — bisa diakses langsung lewat browser (`http://<IP_ESP32>/`) tanpa perlu laptop/Serial Monitor, cocok untuk device yang berjalan dengan daya eksternal (baterai)
- Tampilan lokal di **OLED SSD1306** (suhu, turbidity, pH, status MQTT)
- Indikator buzzer setiap data berhasil terkirim
- Auto-reconnect WiFi & MQTT dengan penanganan timeout
- Data lokasi (GPS) untuk keperluan pemetaan di dashboard *(saat ini masih dummy)*

## Alur Pemrosesan Data

```
Sensor → Raw Data → Median Filter → Threshold → MQTT Publish
```

- Sampling sensor: setiap **1 detik** (disimpan ke buffer, tidak langsung dikirim)
- Publish MQTT: setiap **1 menit** (publish pertama dikirim secepatnya saat device baru menyala, tidak menunggu interval penuh)

## Hardware

| Komponen | Pin |
|---|---|
| ESP32 DevKit (38-pin) | - |
| Sensor suhu DS18B20 | GPIO 15 |
| Sensor turbidity (analog) | GPIO 32 |
| Sensor pH PH-4502C | GPIO 33 |
| Buzzer | GPIO 25 |
| OLED SSD1306 0.96" (I2C) | SDA GPIO 21, SCL GPIO 22 |

## Contoh Payload

```json
{"message_id":"MSG-Aqualyze-Nila-001-0000000005","device_id":"Aqualyze-Nila-001","lokasi":"Kolam Nila A","timestamp":"2026-07-25T18:45:37+07:00","status":{"node_status":"online","ip":"192.168.43.208"},"data":{"suhu":27.56,"status_suhu":"Normal","turbidity_v":1.35,"turbidity_ntu":94,"status_kekeruhan":"Danger","ph":6.94,"status_ph":"Normal"},"location":{"latitude":-6.8914,"longitude":107.610704,"altitude_mdpl":751.98}}
```

## Web Dashboard

Setelah ESP32 terhubung ke WiFi, buka `http://<IP_ESP32>/` di browser (device yang membuka harus berada di jaringan WiFi yang sama). Dashboard menampilkan:

- Status koneksi WiFi/MQTT, uptime, dan free heap
- Data sensor terkini hasil median filter beserta status Normal/Warning/Danger
- Payload MQTT terakhir yang berhasil dikirim
- Log aktivitas (setara Serial Monitor), untuk debugging tanpa perlu kabel USB

## Status Pengembangan

- [x] Sensor suhu (DS18B20)
- [x] Sensor turbidity
- [ ] Sensor pH (masih dummy, menunggu sensor fisik)
- [x] Median filter & threshold status
- [x] Publikasi MQTT ke HiveMQ Cloud
- [x] Web dashboard bawaan ESP32
- [x] Tampilan OLED lokal
- [ ] Data lokasi (GPS) — masih dummy, menunggu modul GPS asli

## Proyek

Dikembangkan sebagai bagian dari Aqualyze, oleh Muhammad Ibrahim Yusuf — mahasiswa D3 Teknik Komputer, UNIKOM.
