# Aqualyze

Sistem monitoring kualitas air real-time untuk kolam ikan nila berbasis ESP32. Aqualyze membaca suhu, kekeruhan (turbidity/NTU), dan pH air, lalu mengirimkan datanya dalam format JSON ke broker MQTT (HiveMQ Cloud) untuk ditampilkan di web dashboard.

## Fitur

- Pembacaan suhu air real-time menggunakan sensor **DS18B20**
- Pembacaan tingkat kekeruhan air (**NTU**) menggunakan sensor turbidity analog
- Pengiriman data via **MQTT over TLS** ke HiveMQ Cloud
- Payload terstruktur dalam format **JSON**, lengkap dengan `message_id`, `device_id`, `timestamp` (ISO 8601), dan status koneksi
- Sinkronisasi waktu otomatis melalui **NTP** (WIB, GMT+7)
- Indikator buzzer setiap data berhasil terkirim
- Auto-reconnect WiFi & MQTT dengan penanganan timeout

## Hardware

| Komponen | Pin |
|---|---|
| ESP32 DevKit (38-pin) | - |
| Sensor suhu DS18B20 | GPIO 15 |
| Sensor turbidity (analog) | GPIO 32 |
| Sensor pH PH-4502C | GPIO 33 |
| Buzzer | GPIO 25 |

## Contoh Payload

```json
{
  "message_id": "MSG-Aqualyze-monitor-0000000001",
  "device_id": "Aqualyze-monitor",
  "timestamp": "2026-07-11T14:23:07+07:00",
  "status": {
    "node_status": "online",
    "ip": "192.168.1.10"
  },
  "data": {
    "suhu": 27.34,
    "turbidity_v": 1.42,
    "turbidity_ntu": 45,
    "ph": 7.12
  }
}
```

## Status Pengembangan

- [x] Sensor suhu (DS18B20)
- [x] Sensor turbidity
- [ ] Sensor pH (masih dummy, menunggu sensor fisik)
- [x] Publikasi MQTT ke HiveMQ Cloud
- [x] Web dashboard *(terhubung terpisah, lihat repo dashboard)*

## Proyek

Dikembangkan sebagai bagian dari Aqualyze, oleh Muhammad Ibrahim Yusuf — mahasiswa D3 Teknik Komputer, UNIKOM.
