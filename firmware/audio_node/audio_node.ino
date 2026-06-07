/*
 * ============================================================
 *  BeeWatch — Audio Node (ESP32-A)
 *  INMP441 (I2S) → WAV → SPIFFS → HTTP POST ke Flask Server
 * ============================================================
 *
 *  Wiring INMP441 → ESP32:
 *  ┌───────────┬─────────────────────────┬────────────┐
 *  │ INMP441   │ Keterangan              │ ESP32      │
 *  ├───────────┼─────────────────────────┼────────────┤
 *  │ VDD       │ Catu daya               │ 3.3V       │
 *  │ GND       │ Ground                  │ GND        │
 *  │ WS        │ Word Select (LRCK)      │ GPIO 15    │
 *  │ SCK       │ Serial Clock (BCLK)     │ GPIO 14    │
 *  │ SD        │ Serial Data             │ GPIO 32    │
 *  │ L/R       │ Channel select (kiri)   │ GND        │
 *  └───────────┴─────────────────────────┴────────────┘
 *
 *  SEBELUM UPLOAD — wajib ganti partition scheme:
 *    Tools → Partition Scheme → No OTA (2MB APP / 2MB SPIFFS)
 *
 *  Alur kerja (setiap 15 menit):
 *    1. Rekam 30 detik audio dari INMP441 via I2S
 *    2. Simpan ke SPIFFS sebagai audio.wav
 *    3. HTTP POST multipart ke Flask /upload/audio
 *    4. Server analisis → kirim notifikasi Telegram jika anomali
 *    5. Hapus file WAV, tunggu sisa interval
 * ============================================================
 */

#include <WiFi.h>
#include <SPIFFS.h>
#include <driver/i2s.h>

// ── Konfigurasi —──────────────────────
#define WIFI_SSID          "nama_wifi"
#define WIFI_PASSWORD      "password_wifi"
#define WIFI_TIMEOUT_MS    15000

#define SERVER_HOST        "10.154.228.94"   // IP VM Ubuntu
#define SERVER_PORT        5000
#define SERVER_PATH        "/upload/audio"
#define HTTP_TIMEOUT_MS    90000             // 90 detik

#define SAMPLE_RATE        16000             // Hz (harus sama dengan model)
#define RECORD_SECONDS     30                // Durasi rekaman per siklus
#define INTERVAL_MINUTES   15                // Interval antar siklus
#define WAV_PATH           "/beewatch.wav"

// Retry
#define WIFI_MAX_RETRY     5
#define HTTP_MAX_RETRY     3
#define HTTP_RETRY_DELAY   3000              // ms antar retry HTTP
// ────────────────────────────────────────────────────────────

// ── Pin I2S INMP441 ─────────────────────────────────────────
#define I2S_PORT           I2S_NUM_0
#define PIN_I2S_WS         15
#define PIN_I2S_SCK        14
#define PIN_I2S_SD         32

// ── LED indikator ────────────────────────────────────────────
#define LED_PIN            2

// ── Internal ─────────────────────────────────────────────────
#define I2S_DMA_BUF_COUNT  4
#define I2S_DMA_BUF_LEN    512
#define CHUNK_SAMPLES      512

static uint32_t cycleStartMs = 0;

// =============================================================
// connectWiFi — sambungkan ke WiFi dengan retry
// =============================================================
bool connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;

    Serial.printf("[WiFi] Menghubungkan ke '%s'", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < WIFI_TIMEOUT_MS) {
        digitalWrite(LED_PIN, !digitalRead(LED_PIN));
        delay(300);
        Serial.print(".");
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Terhubung! IP: %s\n",
                      WiFi.localIP().toString().c_str());
        digitalWrite(LED_PIN, LOW);
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
// setupI2S — inisialisasi I2S untuk INMP441
// INMP441 mengeluarkan data 24-bit dalam frame 32-bit (MSB-justified)
// =============================================================
bool setupI2S() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate          = SAMPLE_RATE,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = I2S_DMA_BUF_COUNT,
        .dma_buf_len          = I2S_DMA_BUF_LEN,
        .use_apll             = false,
        .tx_desc_auto_clear   = false,
        .fixed_mclk           = 0
    };

    i2s_pin_config_t pins = {
        .bck_io_num   = PIN_I2S_SCK,
        .ws_io_num    = PIN_I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = PIN_I2S_SD
    };

    if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) {
        Serial.println("[I2S] Install gagal!");
        return false;
    }
    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) {
        Serial.println("[I2S] Set pin gagal!");
        return false;
    }

    i2s_zero_dma_buffer(I2S_PORT);
    Serial.println("[I2S] OK.");
    return true;
}

// =============================================================
// writeWAVHeader — tulis 44-byte WAV header ke posisi awal file
// Format: PCM 16-bit, mono, SAMPLE_RATE Hz
// =============================================================
void writeWAVHeader(File& f, uint32_t numSamples) {
    const uint16_t channels   = 1;
    const uint16_t bitsPerSmp = 16;
    const uint32_t sr         = SAMPLE_RATE;
    uint32_t dataSize         = numSamples * channels * (bitsPerSmp / 8);
    uint32_t chunkSize        = 36 + dataSize;
    uint32_t byteRate         = sr * channels * bitsPerSmp / 8;
    uint16_t blockAlign       = channels * bitsPerSmp / 8;
    uint32_t subchunk1Size    = 16;
    uint16_t audioFmt         = 1;  // PCM

    f.seek(0);
    f.write((uint8_t*)"RIFF", 4);
    f.write((uint8_t*)&chunkSize,      4);
    f.write((uint8_t*)"WAVE",          4);
    f.write((uint8_t*)"fmt ",          4);
    f.write((uint8_t*)&subchunk1Size,  4);
    f.write((uint8_t*)&audioFmt,       2);
    f.write((uint8_t*)&channels,       2);
    f.write((uint8_t*)&sr,             4);
    f.write((uint8_t*)&byteRate,       4);
    f.write((uint8_t*)&blockAlign,     2);
    f.write((uint8_t*)&bitsPerSmp,     2);
    f.write((uint8_t*)"data",          4);
    f.write((uint8_t*)&dataSize,       4);
}

// =============================================================
// recordToSPIFFS — rekam audio dari INMP441 dan simpan sebagai WAV
// =============================================================
bool recordToSPIFFS() {
    size_t needed = (size_t)SAMPLE_RATE * RECORD_SECONDS * 2 + 44;
    size_t avail  = SPIFFS.totalBytes() - SPIFFS.usedBytes();

    if (avail < needed) {
        Serial.printf("[ERROR] SPIFFS tidak cukup! Tersedia: %u B, Butuh: %u B\n",
                      avail, needed);
        Serial.println("        Tools → Partition Scheme → No OTA (2MB APP/2MB SPIFFS)");
        return false;
    }

    if (SPIFFS.exists(WAV_PATH)) SPIFFS.remove(WAV_PATH);

    File f = SPIFFS.open(WAV_PATH, FILE_WRITE);
    if (!f) {
        Serial.println("[ERROR] Tidak bisa buat file WAV di SPIFFS.");
        return false;
    }

    uint32_t expectedSamples = (uint32_t)SAMPLE_RATE * RECORD_SECONDS;
    writeWAVHeader(f, expectedSamples);

    Serial.printf("[REC] Mulai rekam %d detik...\n", RECORD_SECONDS);
    digitalWrite(LED_PIN, HIGH);

    int32_t  raw32[CHUNK_SAMPLES];
    int16_t  pcm16[CHUNK_SAMPLES];
    uint32_t samplesRecorded = 0;
    size_t   bytesRead;
    bool     success = true;

    while (samplesRecorded < expectedSamples) {
        uint32_t toRead = min((uint32_t)CHUNK_SAMPLES,
                               expectedSamples - samplesRecorded);

        if (i2s_read(I2S_PORT, raw32, toRead * sizeof(int32_t),
                     &bytesRead, pdMS_TO_TICKS(1000)) != ESP_OK) {
            Serial.println("[ERROR] I2S read gagal.");
            success = false;
            break;
        }

        uint32_t got = bytesRead / sizeof(int32_t);
        for (uint32_t i = 0; i < got; i++) {
            // INMP441: data 24-bit di bit 8..31 dari frame 32-bit
            pcm16[i] = (int16_t)(raw32[i] >> 8);
        }

        f.write((uint8_t*)pcm16, got * sizeof(int16_t));
        samplesRecorded += got;

        // Progress setiap 10 detik
        uint32_t elapsed = samplesRecorded / SAMPLE_RATE;
        if (elapsed > 0 && elapsed % 10 == 0 &&
            samplesRecorded % (SAMPLE_RATE * 10) < (uint32_t)CHUNK_SAMPLES) {
            Serial.printf("[REC] %us / %us\n", elapsed, RECORD_SECONDS);
        }
    }

    if (samplesRecorded != expectedSamples) {
        writeWAVHeader(f, samplesRecorded);
    }
    f.close();
    digitalWrite(LED_PIN, LOW);

    if (success) {
        File check = SPIFFS.open(WAV_PATH, FILE_READ);
        Serial.printf("[REC] Selesai! %.2f KB (%u sampel)\n",
                      check.size() / 1024.0f, samplesRecorded);
        check.close();
    }
    return success;
}

// =============================================================
// sendToServer — kirim WAV ke Flask via HTTP POST multipart
// dengan retry otomatis
// =============================================================
bool sendToServer() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[HTTP] WiFi terputus, reconnect...");
        if (!connectWiFiWithRetry()) return false;
    }

    File f = SPIFFS.open(WAV_PATH, FILE_READ);
    if (!f) {
        Serial.println("[ERROR] File WAV tidak ada.");
        return false;
    }
    size_t fileSize = f.size();
    f.close();

    Serial.printf("[HTTP] Mengirim %.2f KB ke %s:%d%s\n",
                  fileSize / 1024.0f, SERVER_HOST, SERVER_PORT, SERVER_PATH);

    const char* boundary = "BeeWatchAudio";
    char partHead[256];
    snprintf(partHead, sizeof(partHead),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"audio\"; filename=\"audio.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        boundary);
    char partTail[64];
    snprintf(partTail, sizeof(partTail), "\r\n--%s--\r\n", boundary);

    size_t totalLen = strlen(partHead) + fileSize + strlen(partTail);

    for (int attempt = 1; attempt <= HTTP_MAX_RETRY; attempt++) {
        WiFiClient client;
        client.setTimeout(HTTP_TIMEOUT_MS / 1000);

        if (!client.connect(SERVER_HOST, SERVER_PORT)) {
            Serial.printf("[HTTP] Koneksi gagal (attempt %d/%d)\n",
                          attempt, HTTP_MAX_RETRY);
            if (attempt < HTTP_MAX_RETRY) delay(HTTP_RETRY_DELAY);
            continue;
        }

        // Kirim HTTP headers
        client.printf("POST %s HTTP/1.1\r\n", SERVER_PATH);
        client.printf("Host: %s:%d\r\n", SERVER_HOST, SERVER_PORT);
        client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary);
        client.printf("Content-Length: %u\r\n", (unsigned int)totalLen);
        client.print("Connection: close\r\n\r\n");
        client.print(partHead);

        // Stream file WAV
        File fSend = SPIFFS.open(WAV_PATH, FILE_READ);
        uint8_t buf[512];
        size_t  remaining = fileSize;
        while (remaining > 0 && fSend.available()) {
            size_t chunk = min(remaining, sizeof(buf));
            size_t got   = fSend.read(buf, chunk);
            if (got == 0) break;
            client.write(buf, got);
            remaining -= got;
            yield();
        }
        fSend.close();
        client.print(partTail);

        // Baca response
        unsigned long timeout = millis();
        while (!client.available() && millis() - timeout < HTTP_TIMEOUT_MS) {
            delay(100);
        }

        String statusLine = client.readStringUntil('\n');
        statusLine.trim();
        Serial.printf("[HTTP] Status: %s\n", statusLine.c_str());

        // Skip headers
        while (client.available()) {
            String line = client.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) break;
        }

        // Baca body
        String body = "";
        while (client.available()) body += (char)client.read();
        client.stop();

        if (body.length() > 0) {
            Serial.printf("[HTTP] Response: %s\n", body.c_str());
        }

        bool ok = statusLine.indexOf("200") >= 0;
        if (ok) {
            bool isAnomaly = body.indexOf("\"is_anomaly\":true") >= 0 ||
                             body.indexOf("\"is_anomaly\": true") >= 0;
            if (isAnomaly) {
                String sev = body.indexOf("\"critical\"") >= 0 ? "CRITICAL" :
                             body.indexOf("\"warning\"")  >= 0 ? "WARNING"  : "ANOMALI";
                Serial.printf("[!!!] ANOMALI — severity: %s\n", sev.c_str());
            } else {
                Serial.println("[OK] Kondisi akustik: NORMAL.");
            }
            return true;
        }

        Serial.printf("[WARN] Server tidak OK (attempt %d/%d)\n",
                      attempt, HTTP_MAX_RETRY);
        if (attempt < HTTP_MAX_RETRY) {
            Serial.printf("[HTTP] Retry dalam %d detik...\n", HTTP_RETRY_DELAY / 1000);
            delay(HTTP_RETRY_DELAY);
        }
    }

    Serial.println("[ERROR] Semua percobaan kirim gagal.");
    return false;
}

// =============================================================
// setup
// =============================================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("================================================");
    Serial.println("  BeeWatch — Audio Node (ESP32-A)");
    Serial.printf ("  Sample rate : %d Hz\n", SAMPLE_RATE);
    Serial.printf ("  Durasi      : %d detik per rekaman\n", RECORD_SECONDS);
    Serial.printf ("  Interval    : %d menit\n", INTERVAL_MINUTES);
    Serial.println("================================================");

    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);

    // SPIFFS
    if (!SPIFFS.begin(true)) {
        Serial.println("[ERROR] SPIFFS gagal! Cek partition scheme.");
        while (true) {
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));
            delay(100);
        }
    }
    Serial.printf("[SPIFFS] OK — Tersedia: %.2f MB\n",
                  (SPIFFS.totalBytes() - SPIFFS.usedBytes()) / 1048576.0f);

    // WiFi
    if (!connectWiFiWithRetry()) {
        Serial.println("[WARN] WiFi gagal saat startup, akan coba lagi di loop.");
    }

    // I2S
    if (!setupI2S()) {
        Serial.println("[ERROR] I2S gagal! Cek wiring INMP441.");
        while (true) {
            digitalWrite(LED_PIN, !digitalRead(LED_PIN));
            delay(200);
        }
    }

    Serial.println("[READY] Setup selesai.\n");
    cycleStartMs = millis();
}

// =============================================================
// loop — siklus 15 menit: rekam → kirim → tunggu
// =============================================================
void loop() {
    cycleStartMs = millis();
    Serial.printf("\n=== SIKLUS BARU [%lu ms] ===\n", cycleStartMs);

    // 1. Rekam
    bool recorded = recordToSPIFFS();
    digitalWrite(LED_PIN, LOW);

    // 2. Kirim
    if (!recorded) {
        Serial.println("[WARN] Rekam gagal, skip pengiriman.");
    } else {
        sendToServer();

        // 3. Cleanup
        if (SPIFFS.exists(WAV_PATH)) {
            SPIFFS.remove(WAV_PATH);
            Serial.println("[SPIFFS] File WAV dihapus.");
        }
    }

    // 4. Tunggu sisa interval
    uint32_t elapsed    = millis() - cycleStartMs;
    uint32_t intervalMs = (uint32_t)INTERVAL_MINUTES * 60 * 1000UL;

    if (elapsed < intervalMs) {
        uint32_t waitMs = intervalMs - elapsed;
        Serial.printf("[WAIT] Menunggu %u menit %u detik...\n",
                      waitMs / 60000, (waitMs % 60000) / 1000);
        delay(waitMs);
    } else {
        Serial.println("[WARN] Proses melebihi interval, langsung siklus berikutnya.");
    }
}