/*
 * ============================================================
 *  BeeWatch — Sensor Node (ESP32-B)
 *  BME280 (I2C) → JSON → HTTP POST ke Flask Server
 * ============================================================
 *
 *  Wiring BME280 → ESP32:
 *  ┌───────────┬─────────────────────────┬────────────┐
 *  │ BME280    │ Keterangan              │ ESP32      │
 *  ├───────────┼─────────────────────────┼────────────┤
 *  │ VIN       │ Catu daya               │ 3.3V       │
 *  │ GND       │ Ground                  │ GND        │
 *  │ SDA       │ I2C Data                │ GPIO 25    │
 *  │ SCL       │ I2C Clock               │ GPIO 26    │
 *  └───────────┴─────────────────────────┴────────────┘
 *
 *  Catatan: Pin SDO disambung ke GND → alamat I2C = 0x76
 *           Pin SDO disambung ke 3.3V → alamat I2C = 0x77
 *
 *  Alur kerja (setiap 15 menit):
 *    1. Baca suhu, kelembaban, tekanan dari BME280
 *    2. Terapkan offset kalibrasi tekanan
 *    3. HTTP POST JSON ke Flask /upload/sensor
 *    4. Server inferensi → kirim notifikasi Telegram jika anomali
 * ============================================================
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_BME280.h>

// ── Konfigurasi —──────────────────────
#define WIFI_SSID          "nama_wifi"
#define WIFI_PASSWORD      "password_wifi"
#define SERVER_URL         "http://10.154.228.94:5000/upload/sensor"  // IP VM Ubuntu
#define INTERVAL_MS        (15 * 60 * 1000)  // interval kirim (15 menit)

// Pin I2C custom
#define I2C_SDA            25
#define I2C_SCL            26

// Kalibrasi tekanan — sesuaikan dengan lokasi deployment
// Cara ukur: nilai_aktual - nilai_terbaca (cek di weather app)
#define PRESSURE_OFFSET    89.0f  // hPa

// Retry
#define WIFI_MAX_RETRY     5
#define HTTP_MAX_RETRY     3
#define HTTP_RETRY_DELAY   3000   // ms antar retry HTTP
// ────────────────────────────────────────────────────────────

Adafruit_BME280 bme;
bool sensorOK = false;

// =============================================================
// connectWiFi — sambungkan ke WiFi, return true jika berhasil
// =============================================================
bool connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;

    Serial.printf("[WiFi] Menghubungkan ke '%s'", WIFI_SSID);
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 20) {
        delay(500);
        Serial.print(".");
        retry++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Terhubung! IP: %s\n",
                      WiFi.localIP().toString().c_str());
        return true;
    }

    Serial.println("[WiFi] Gagal terhubung.");
    return false;
}

bool connectWiFiWithRetry() {
    for (int i = 0; i < WIFI_MAX_RETRY; i++) {
        if (connectWiFi()) return true;
        Serial.printf("[WiFi] Retry %d/%d...\n", i + 1, WIFI_MAX_RETRY);
        delay(3000);
    }
    return false;
}

// =============================================================
// setup
// =============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("================================================");
    Serial.println("  BeeWatch — Sensor Node (ESP32-B)");
    Serial.printf ("  Interval    : %d menit\n", INTERVAL_MS / 60000);
    Serial.printf ("  Offset P    : %.1f hPa\n", PRESSURE_OFFSET);
    Serial.println("================================================");

    // Inisialisasi I2C di pin custom
    Wire.begin(I2C_SDA, I2C_SCL);

    // Inisialisasi BME280 — coba 0x76 dulu, fallback ke 0x77
    if (!bme.begin(0x76)) {
        Serial.println("[BME280] Tidak ditemukan di 0x76, coba 0x77...");
        if (!bme.begin(0x77)) {
            Serial.println("[BME280] Tidak ditemukan! Cek wiring.");
            sensorOK = false;
        } else {
            Serial.println("[BME280] Ditemukan di 0x77");
            sensorOK = true;
        }
    } else {
        Serial.println("[BME280] Ditemukan di 0x76");
        sensorOK = true;
    }

    if (sensorOK) {
        bme.setSampling(
            Adafruit_BME280::MODE_NORMAL,
            Adafruit_BME280::SAMPLING_X1,
            Adafruit_BME280::SAMPLING_X1,
            Adafruit_BME280::SAMPLING_X1,
            Adafruit_BME280::FILTER_OFF,
            Adafruit_BME280::STANDBY_MS_1000
        );
        delay(2000);  // tunggu sensor stabil
    }

    // WiFi
    if (!connectWiFiWithRetry()) {
        Serial.println("[WARN] WiFi gagal saat startup, akan coba lagi di loop.");
    }

    Serial.println("[READY] Setup selesai.\n");
}

// =============================================================
// sendToServer — kirim data sensor ke Flask via HTTP POST JSON
// dengan retry otomatis
// =============================================================
void sendToServer(float temperature, float humidity, float pressure) {
    // Pastikan WiFi terhubung
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Terputus, mencoba reconnect...");
        if (!connectWiFiWithRetry()) {
            Serial.println("[WiFi] Gagal reconnect, skip pengiriman.");
            return;
        }
    }

    // Buat payload JSON
    StaticJsonDocument<200> doc;
    doc["temperature"] = round(temperature * 10) / 10.0;
    doc["humidity"]    = round(humidity    * 10) / 10.0;
    doc["pressure"]    = round(pressure    * 10) / 10.0;

    String payload;
    serializeJson(doc, payload);
    Serial.println("[HTTP] Mengirim: " + payload);

    // Kirim dengan retry
    bool success = false;
    for (int attempt = 1; attempt <= HTTP_MAX_RETRY; attempt++) {
        HTTPClient http;
        http.begin(SERVER_URL);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(10000);

        int responseCode = http.POST(payload);

        if (responseCode > 0) {
            Serial.printf("[HTTP] Response %d: %s\n",
                          responseCode, http.getString().c_str());
            success = true;
            http.end();
            break;
        } else {
            Serial.printf("[HTTP] Gagal (attempt %d/%d), error: %d\n",
                          attempt, HTTP_MAX_RETRY, responseCode);
            http.end();
            if (attempt < HTTP_MAX_RETRY) {
                Serial.printf("[HTTP] Retry dalam %d detik...\n",
                              HTTP_RETRY_DELAY / 1000);
                delay(HTTP_RETRY_DELAY);
            }
        }
    }

    if (!success) {
        Serial.println("[HTTP] Semua percobaan gagal, data tidak terkirim.");
    }
}

// =============================================================
// loop — baca sensor → kirim → tunggu interval
// =============================================================
void loop() {
    if (!sensorOK) {
        Serial.println("[ERROR] Sensor tidak OK, skip pembacaan.");
        delay(INTERVAL_MS);
        return;
    }

    // Baca sensor
    float temperature = bme.readTemperature();
    float humidity    = bme.readHumidity();
    float pressure    = (bme.readPressure() / 100.0F) + PRESSURE_OFFSET;

    // Validasi pembacaan
    if (isnan(temperature) || isnan(humidity) || isnan(pressure)) {
        Serial.println("[ERROR] Pembacaan sensor tidak valid, skip.");
        delay(INTERVAL_MS);
        return;
    }

    Serial.println("─────────────────────────────");
    Serial.printf("[BME280] Suhu       : %.1f °C\n",  temperature);
    Serial.printf("[BME280] Kelembaban : %.1f %%\n",  humidity);
    Serial.printf("[BME280] Tekanan    : %.1f hPa\n", pressure);
    Serial.println("─────────────────────────────");

    sendToServer(temperature, humidity, pressure);

    Serial.printf("[WAIT] Menunggu %d menit...\n\n", INTERVAL_MS / 60000);
    delay(INTERVAL_MS);
}