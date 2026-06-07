# 🐝 BeeWatch: Smart Hive Integrated Monitoring System

BeeWatch adalah sistem pemantauan sarang lebah cerdas berbasis **IoT dan Machine Learning** yang mendeteksi anomali kondisi sarang secara *real-time* menggunakan dua modalitas: **sensor lingkungan** (suhu, kelembaban, tekanan) dan **audio akustik** dari dalam sarang. Sistem ini dibangun di atas dua node ESP32 yang bekerja secara paralel, mengirimkan data ke server Flask yang menjalankan dua model autoencoder untuk menghasilkan satu *combined anomaly score*.

---

## Daftar Isi

1. [Arsitektur Sistem](#arsitektur-sistem)
2. [Firmware ESP32](#firmware-esp32)
3. [Server Flask](#server-flask)
4. [Model Sensor Autoencoder](#model-sensor-autoencoder)
5. [Model Audio Autoencoder](#model-audio-autoencoder)
6. [Penggabungan Skor](#penggabungan-skor-combined-score)
7. [Notifikasi Telegram](#notifikasi-telegram)
8. [Struktur Proyek](#struktur-proyek)
9. [Instalasi dan Menjalankan Server](#instalasi-dan-menjalankan-server)
10. [API Endpoint](#api-endpoint)
11. [Training Model](#training-model)
12. [Dependensi](#dependensi)

---

## Arsitektur Sistem

```
┌─────────────────────┐      ┌─────────────────────┐
│   ESP32 Sensor Node │      │   ESP32 Audio Node  │
│                     │      │                     │
│  BME280             │      │  INMP441 (I2S mic)  │
│  suhu / kelembaban  │      │  rekam WAV 30 detik │
│  / tekanan          │      │  setiap 15 menit    │
└────────┬────────────┘      └──────────┬──────────┘
         │  POST /upload/sensor         │  POST /upload/audio
         │  (JSON)                      │  (multipart WAV)
         └──────────────┬───────────────┘
                        │
               ┌────────▼────────┐
               │  Flask Server   │
               │   (app.py)      │
               │                 │
               │  Sensor AE  ────┤──► sensor_score
               │  Audio AE   ────┤──► audio_score
               │                 │
               │ Combined Score  │
               │ (50:50 weighted │
               │  dalam ±5 mnt)  │
               └────────┬────────┘
                        │
           ┌────────────┼────────────┐
           │                         │
    ┌──────▼──────┐         ┌────────▼────────┐
    │  SQLite DB  │         │  Telegram Bot   │
    │ (beewatch   │         │  📊 Laporan     │
    │   .db)      │         │  🚨 Alert       │
    └─────────────┘         └─────────────────┘
```

### Alur Deteksi Anomali

1. **ESP32 Sensor** membaca BME280 setiap 15 menit dan mengirim data JSON ke `/upload/sensor`
2. **ESP32 Audio** merekam audio 30 detik via INMP441, menyimpannya sebagai WAV di SPIFFS, lalu mengirim ke `/upload/audio`
3. Server menyimpan pembacaan sensor dan audio terbaru; keduanya digabungkan jika selisih waktu tidak lebih dari 5 menit
4. Combined score dihitung dengan rumus `0.5 × sensor_score + 0.5 × audio_score`
5. Jika `combined_score > 0.5`, status ditetapkan sebagai **ANOMALI** dan laporan dikirim ke Telegram
6. Jika anomali terdeteksi secara berturut-turut sebanyak N kali (default: 2), server mengirim **alert darurat**

---

## Firmware ESP32

Firmware tersimpan di folder `firmware/` dan ditulis untuk **Arduino IDE** (format `.ino`). Kedua node bekerja secara independen dan mengirim data ke server Flask yang sama.

### Sensor Node

**File:** `firmware/sensor_node/sensor_node.ino`  
**Hardware:** ESP32 + BME280

**Wiring BME280 ke ESP32:**

| Pin BME280 | Fungsi | Pin ESP32 |
|---|---|---|
| VIN | Catu daya | 3.3V |
| GND | Ground | GND |
| SDA | I2C Data | GPIO 25 |
| SCL | I2C Clock | GPIO 26 |

> Jika pin SDO dihubungkan ke GND, alamat I2C yang digunakan adalah `0x76`. Jika dihubungkan ke 3.3V, alamatnya menjadi `0x77`.

**Parameter konfigurasi:**

| Parameter | Nilai | Keterangan |
|---|---|---|
| `INTERVAL_MS` | 15 menit | Interval pengiriman data ke server |
| `PRESSURE_OFFSET` | 89.0 hPa | Offset kalibrasi tekanan, sesuaikan dengan lokasi |
| `WIFI_MAX_RETRY` | 5 | Jumlah maksimum percobaan ulang koneksi WiFi |
| `HTTP_MAX_RETRY` | 3 | Jumlah maksimum percobaan ulang HTTP POST |
| `HTTP_RETRY_DELAY` | 3000 ms | Jeda waktu antar percobaan HTTP |

**Alur kerja setiap siklus 15 menit:**
1. Baca nilai suhu, kelembaban, dan tekanan dari BME280
2. Tambahkan offset kalibrasi pada nilai tekanan
3. Kirim payload JSON ke Flask via HTTP POST
4. Tunggu hingga interval berikutnya

**Contoh payload JSON yang dikirim:**
```json
{
  "temperature": 35.5,
  "humidity": 62.3,
  "pressure": 1008.1
}
```

**Konfigurasi yang wajib diubah sebelum flashing:**
```cpp
#define WIFI_SSID      "nama_wifi"
#define WIFI_PASSWORD  "password_wifi"
#define SERVER_URL     "http://<IP_SERVER>:5000/upload/sensor"
```

**Library yang dibutuhkan:**
- `WiFi.h` (bawaan ESP32 core)
- `HTTPClient.h` (bawaan ESP32 core)
- `ArduinoJson` by Benoit Blanchon
- `Adafruit BME280 Library`

---

### Audio Node

**File:** `firmware/audio_node/audio_node.ino`  
**Hardware:** ESP32 + INMP441

**Wiring INMP441 ke ESP32:**

| Pin INMP441 | Fungsi | Pin ESP32 |
|---|---|---|
| VDD | Catu daya | 3.3V |
| GND | Ground | GND |
| WS | Word Select / LRCK | GPIO 15 |
| SCK | Serial Clock / BCLK | GPIO 14 |
| SD | Serial Data | GPIO 32 |
| L/R | Pemilih channel (kiri) | GND |

> **Sebelum upload firmware:** Ubah partition scheme di Arduino IDE melalui menu `Tools > Partition Scheme > No OTA (2MB APP / 2MB SPIFFS)`. Langkah ini diperlukan agar SPIFFS memiliki ruang yang cukup untuk menyimpan file WAV.

**Parameter konfigurasi:**

| Parameter | Nilai | Keterangan |
|---|---|---|
| `SAMPLE_RATE` | 16.000 Hz | Harus sama dengan konfigurasi model audio |
| `RECORD_SECONDS` | 30 detik | Durasi rekaman per siklus |
| `INTERVAL_MINUTES` | 15 menit | Interval antar siklus rekam dan kirim |
| `WAV_PATH` | `/beewatch.wav` | Lokasi file sementara di SPIFFS |
| `HTTP_TIMEOUT_MS` | 90.000 ms | Timeout untuk proses upload file WAV |
| `I2S_PORT` | `I2S_NUM_0` | Port I2S yang digunakan |
| `LED_PIN` | GPIO 2 | LED indikator status bawaan ESP32 |

**Alur kerja setiap siklus 15 menit:**
1. Rekam audio 30 detik dari INMP441 via I2S
2. Simpan ke SPIFFS sebagai `beewatch.wav` (format 16-bit PCM, mono, 16 kHz)
3. Upload file WAV ke Flask via HTTP POST multipart
4. Hapus file WAV dari SPIFFS untuk membebaskan memori
5. Tunggu hingga sisa interval 15 menit terpenuhi

**Indikator LED:**
- Berkedip cepat setiap 100 ms: terjadi error pada SPIFFS saat startup
- Berkedip setiap 200 ms: terjadi error pada I2S saat startup
- LED mati setelah pengiriman: siklus selesai, ESP32 menunggu interval berikutnya

**Konfigurasi yang wajib diubah sebelum flashing:**
```cpp
#define WIFI_SSID      "nama_wifi"
#define WIFI_PASSWORD  "password_wifi"
#define SERVER_HOST    "<IP_SERVER>"
#define SERVER_PORT    5000
```

**Library yang dibutuhkan:**
- `WiFi.h` (bawaan ESP32 core)
- `SPIFFS.h` (bawaan ESP32 core)
- `driver/i2s.h` (bawaan ESP32 core)

---

## Server Flask

Server berjalan dari `server/app.py` dan terdiri dari empat modul:

| File | Tanggung Jawab |
|---|---|
| `app.py` | Entry point Flask, routing endpoint HTTP, dan logika combined score beserta streak anomali |
| `inference.py` | Kelas `BeeWatchInference` yang memuat kedua model dan menghitung skor dari tiap modalitas |
| `database.py` | Inisialisasi SQLite dan pencatatan setiap pembacaan ke tabel `readings` |
| `notifier.py` | Kelas `TelegramNotifier` yang mengirim laporan rutin dan peringatan darurat |

Konfigurasi server disimpan di file `server/.env`:

```env
TELEGRAM_BOT_TOKEN=<token_dari_BotFather>
TELEGRAM_CHAT_ID=<chat_id_atau_group_id>
PORT=5000
ANOMALY_CONSECUTIVE_COUNT=2
```

---

## Model Sensor Autoencoder

Notebook training tersedia di `sensor_training.ipynb`.

### Dataset

- **Sumber:** [Kaggle: Beehive Sounds by annajyang](https://www.kaggle.com/datasets/annajyang/beehive-sounds)
- **File:** `all_data_updated.csv` berisi 1.275 baris data lingkungan sarang dari 4 koloni lebah madu Eropa
- **Fitur yang digunakan:** `hive temp`, `hive humidity`, `hive pressure` (kolom `hive weight` tidak disertakan)
- **Pendekatan:** Unsupervised. Label `target` tidak digunakan saat training dan hanya dipakai sebagai referensi evaluasi

### Distribusi Fitur Sensor

Distribusi ketiga fitur sensor memperlihatkan rentang kondisi sarang yang normal: suhu berkisar 15 hingga 55 °C dengan puncak di sekitar 28 °C, kelembaban antara 15 hingga 90 %RH, dan tekanan pada rentang 1004 hingga 1016 hPa.

![Distribusi Fitur Sensor](training/sensor_distribution.png)

### Analisis Korelasi Sensor

Scatter matrix antar-fitur memperlihatkan korelasi negatif antara suhu dan kelembaban, yaitu kondisi yang wajar dan diharapkan pada sarang lebah aktif.

![Korelasi Sensor](training/sensor_correlation.png)

### Arsitektur Model

Model sensor menggunakan **autoencoder simetris** dengan bottleneck 2 dimensi. Ukuran bottleneck dibuat kecil agar representasi laten dapat divisualisasikan secara langsung.

```
Input (3)
   └─► Dense(16) + ReLU
           └─► Dense(8) + ReLU
                   └─► Dense(2)  [BOTTLENECK]
                           └─► Dense(8) + ReLU
                                   └─► Dense(16) + ReLU
                                           └─► Output (3)
```

- **Optimizer:** Adam dengan learning rate 0.001
- **Loss:** Mean Squared Error (MSE)
- **Normalisasi:** StandardScaler di-fit hanya pada data train, lalu disimpan sebagai `sensor_scaler.pkl`
- **Threshold anomali:** Persentil 95 dari reconstruction error pada data train

### Kurva Training

Model mencapai konvergensi pada epoch ke-123 yang ditentukan oleh EarlyStopping dengan patience 20. Validation loss secara konsisten berada di bawah training loss, menandakan tidak adanya overfitting.

![Kurva Training Sensor](training/sensor_training_loss.png)

### Distribusi Reconstruction Error

Sebagian besar sampel memiliki error yang rendah dan mendekati nol. Threshold P95 yang ditetapkan pada nilai **0.3848** berarti hanya 5% sampel dengan error tertinggi yang diklasifikasikan sebagai anomali.

![Distribusi Error Sensor](training/sensor_error_distribution.png)

### Evaluasi Model

Panel kiri menunjukkan distribusi error antara data train dan validasi yang hampir identik, mengkonfirmasi bahwa model tidak overfit. Panel kanan memperlihatkan representasi bottleneck 2D yang diwarnai berdasarkan reconstruction error: sampel dengan error tinggi tampak terisolasi di tepi cluster, menandakan model berhasil memisahkan kondisi anomali dari kondisi normal.

![Evaluasi Sensor](training/sensor_evaluation.png)

### Visualisasi Bottleneck

Representasi bottleneck 2D diwarnai per kelas target (0 hingga 5, merepresentasikan status koloni). Meskipun training dilakukan secara unsupervised, bottleneck secara alami mengelompokkan kondisi sarang yang berbeda. Ini menjadi validasi bahwa model mempelajari struktur yang bermakna dari data.

![Bottleneck Sensor](training/sensor_bottleneck.png)

### Artefak Output

| File | Fungsi |
|---|---|
| `sensor_autoencoder.keras` | Model Keras untuk inferensi di server |
| `sensor_scaler.pkl` | StandardScaler yang di-fit pada data train |
| `sensor_threshold.npy` | Nilai threshold P95 reconstruction error |

---

## Model Audio Autoencoder

Notebook training tersedia di `audio_training.ipynb`, dijalankan di Kaggle Notebook dengan akselerasi GPU T4.

### Dataset

- **Sumber:** [Kaggle: Beehive Sounds by annajyang](https://www.kaggle.com/datasets/annajyang/beehive-sounds)
- **File audio:** 7.100 file WAV dari 4 koloni lebah madu Eropa, direkam selama kurang lebih 1 menit setiap 15 menit
- **Pendekatan:** Unsupervised murni. Semua file WAV diperlakukan sebagai representasi pola akustik normal yang harus dipelajari model

### Ekstraksi Fitur Audio

Setiap file WAV dipotong menjadi segmen 2 detik dengan sample rate 16.000 Hz. Dari tiap segmen diekstrak fitur akustik, kemudian dirata-ratakan lintas segmen untuk menghasilkan satu vektor per file:

| Fitur | Parameter | Dimensi |
|---|---|---|
| MFCC mean | 13 koefisien | 13 |
| MFCC std | 13 koefisien | 13 |
| Mel-Spectrogram mean | 20 band dari 128 mel | 20 |
| Mel-Spectrogram std | 20 band dari 128 mel | 20 |
| Spectral Centroid mean + std | - | 2 |
| Spectral Rolloff mean + std | - | 2 |
| Zero Crossing Rate mean + std | - | 2 |
| **Total** | | **72** |

Parameter ekstraktor: `n_fft=512`, `hop_length=256`, `n_mels=128`

### Distribusi Fitur Audio

Distribusi 12 koefisien MFCC pertama memperlihatkan bentuk yang relatif normal dan simetris. Ini merupakan kondisi yang baik untuk pelatihan autoencoder karena fitur tidak terlalu skewed.

![Distribusi Fitur Audio](training/audio_feature_distribution.png)

### PCA Fitur Audio Sebelum Normalisasi

Sebelum normalisasi diterapkan, PC1 mendominasi 98,3% dari total variance. Hal ini terjadi karena perbedaan skala yang besar antar-fitur dan menjadi alasan utama mengapa StandardScaler wajib diterapkan sebelum training.

![PCA Audio Sebelum Normalisasi](training/audio_pca_raw.png)

### Arsitektur Model

Model audio menggunakan **progressive compression** dengan bottleneck 32 dimensi:

```
Input (72)
   └─► Dense(64) + BatchNorm + ReLU + Dropout(0.1)
           └─► Dense(32)  [BOTTLENECK, aktivasi ReLU]
                   └─► Dense(64) + BatchNorm + ReLU + Dropout(0.1)
                           └─► Output (72)
```

- **Optimizer:** Adam (lr = 0.001) dengan `ReduceLROnPlateau` factor 0.5 dan patience 8
- **Loss:** Mean Squared Error (MSE)
- **Split data:** 80% train / 20% validasi
- **Maksimum epoch:** 250, dihentikan lebih awal oleh `EarlyStopping` dengan patience 20

### Kurva Training

Model mencapai konvergensi pada epoch ke-180. Validation loss (garis merah) secara konsisten berada di bawah training loss, menunjukkan model tidak overfit. Penurunan loss yang mulus mengkonfirmasi bahwa arsitektur progressive compression bekerja efektif untuk data akustik 72 dimensi.

![Kurva Training Audio](training/audio_training_loss.png)

### Distribusi Reconstruction Error

Panel kiri memperlihatkan distribusi error yang sangat right-skewed: sebagian besar sampel memiliki error sangat rendah dengan ekor panjang ke kanan. Threshold P95 ditetapkan pada **0.6127**.

Panel kanan menampilkan sorted error curve yang membentuk pola "hockey stick", yaitu pola ideal di mana error baru naik tajam pada 5% sampel terakhir.

![Distribusi Error Audio](training/audio_error_distribution.png)

### Evaluasi Model

Panel kiri (boxplot) memperlihatkan distribusi error train dan validasi yang hampir identik dengan median mendekati nol, mengkonfirmasi tidak adanya overfitting. Sampel di atas garis threshold adalah kandidat anomali.

Panel kanan menampilkan distribusi audio anomaly score dalam rentang 0 hingga 1. Mayoritas sampel mendapat skor mendekati nol (normal). Threshold score ditetapkan pada **0.8108** dan hanya sampel dengan skor di atasnya yang akan memicu pengiriman notifikasi.

![Evaluasi Audio](training/audio_evaluation.png)

### Visualisasi Ruang Laten

Representasi ruang laten 32 dimensi diproyeksikan ke 2D menggunakan PCA (PC1 = 31,9%, PC2 = 10,5%). Sampel dengan pola akustik normal (kuning muda) membentuk cluster padat di sisi kiri, sementara sampel anomali (oranye dan merah) memencar ke kanan. Pemisahan ini menjadi bukti bahwa model berhasil mempelajari manifold pola akustik normal sarang lebah.

![Bottleneck PCA Audio](training/audio_bottleneck_pca.png)

### Artefak Output

| File | Fungsi |
|---|---|
| `audio_autoencoder.keras` | Model Keras untuk inferensi di server |
| `audio_scaler.pkl` | StandardScaler yang di-fit pada 7.100 sampel audio |
| `model_config.pkl` | Konfigurasi ekstraktor fitur (SR, N_MFCC, N_MELS, N_FFT, dll.) |
| `thresholds.pkl` | Threshold P95, mean, dan std reconstruction error |

---

## Penggabungan Skor (Combined Score)

Setelah kedua pembacaan tersedia dalam jendela waktu 5 menit, server menghitung combined score sebagai berikut:

```
sensor_score  = clip(sensor_error / sensor_threshold, 0.0, 1.0)

z             = (audio_raw_error - audio_mean) / (audio_std + ε)
audio_score   = clip(z / 6.0 + 0.5, 0.0, 1.0)

combined_score = 0.5 × sensor_score + 0.5 × audio_score
is_anomaly     = combined_score > 0.5
```

Penjelasan tiap komponen:

- **`sensor_score`** dinormalisasi relatif terhadap threshold. Nilai 0 berarti kondisi sempurna normal, nilai 1 berarti error setara atau melebihi threshold.
- **`audio_score`** dihitung dari Z-score reconstruction error audio, lalu di-mapping ke rentang [0, 1]. Error yang sama dengan rata-rata training menghasilkan skor 0.5, sedangkan error yang berada 3 standar deviasi di atas rata-rata menghasilkan skor mendekati 1.0.
- Kedua skor digabungkan dengan bobot **50:50**.

Setelah combined score selesai dihitung, buffer `latest_sensor` dan `latest_audio` direset sehingga setiap pasangan pembacaan hanya diproses satu kali.

### Logika Streak dan Alert Darurat

```
anomaly_streak += 1  # setiap kali anomali terdeteksi
anomaly_streak  = 0  # direset setiap kali kondisi normal

if anomaly_streak >= ANOMALY_CONSECUTIVE_COUNT:
    kirim emergency alert ke Telegram
    reset anomaly_streak ke 0
```

---

## Notifikasi Telegram

Server mengirim dua jenis pesan ke bot Telegram:

**Laporan Rutin** dikirim setiap kali combined score berhasil dihitung:

```
📊 [BeeWatch] Laporan Rutin
🕐 Waktu          : 2025-01-01 10:00:00
🌡️ Suhu           : 35.5 °C
💧 Kelembaban     : 62.3 %RH
📈 Tekanan        : 1008.1 hPa
🌿 Sensor Score   : 0.3200
🔊 Audio Score    : 0.2800
⚖️ Combined Score : 0.3000
📌 Status         : ✅ NORMAL
```

**Peringatan Darurat** dikirim saat anomali terdeteksi N kali berturut-turut:

```
🚨 [BeeWatch] PERINGATAN ANOMALI
🕐 Waktu          : 2025-01-01 10:15:00
...
❗ Status          : ANOMALI TERDETEKSI
🔍 Rekomendasi    : Segera periksa kondisi sarang Anda.
```

---

## Struktur Proyek

```
beewatch/
├── firmware/
│   ├── sensor_node/
│   │   └── sensor_node.ino     # ESP32 + BME280: baca sensor dan kirim JSON
│   └── audio_node/
│       └── audio_node.ino      # ESP32 + INMP441: rekam WAV dan kirim ke server
├── server/
│   ├── app.py                  # Flask server, routing, dan logika combined score
│   ├── inference.py            # Kelas BeeWatchInference: load model dan hitung skor
│   ├── database.py             # Inisialisasi dan pencatatan ke SQLite
│   ├── notifier.py             # TelegramNotifier: laporan rutin dan peringatan darurat
│   └── .env                    # Konfigurasi rahasia (tidak di-commit)
├── training/
│   ├── sensor_training.ipynb   # Notebook training Sensor Autoencoder
│   ├── audio_training.ipynb    # Notebook training Audio Autoencoder (Kaggle GPU)
│   ├── sensor_distribution.png
│   ├── sensor_correlation.png
│   ├── sensor_training_loss.png
│   ├── sensor_error_distribution.png
│   ├── sensor_evaluation.png
│   ├── sensor_bottleneck.png
│   ├── audio_feature_distribution.png
│   ├── audio_pca_raw.png
│   ├── audio_training_loss.png
│   ├── audio_error_distribution.png
│   ├── audio_evaluation.png
│   └── audio_bottleneck_pca.png
├── models/                     # Artefak model (tidak di-commit, diisi setelah training)
│   ├── sensor_autoencoder.keras
│   ├── sensor_scaler.pkl
│   ├── sensor_threshold.npy
│   ├── audio_autoencoder.keras
│   ├── audio_scaler.pkl
│   ├── model_config.pkl
│   └── thresholds.pkl
├── data/                       # SQLite database (dibuat otomatis saat server pertama kali jalan)
│   └── beewatch.db
├── sensor_training.ipynb
├── audio_training.ipynb
└── requirements.txt
```

---

## Instalasi dan Menjalankan Server

### Prasyarat

- Python 3.10 atau lebih baru
- Artefak model tersedia di folder `models/` (dihasilkan dari training notebook)
- Bot Telegram sudah dibuat melalui BotFather dan token sudah tersedia

### Clone dan Install Dependensi

```bash
git clone https://github.com/<username>/beewatch.git
cd beewatch
python -m venv venv

# Windows
venv\Scripts\activate

# Linux / macOS
source venv/bin/activate

pip install -r requirements.txt
```

### Konfigurasi

Buat file `server/.env` dan isi dengan nilai yang sesuai:

```env
TELEGRAM_BOT_TOKEN=<token_dari_BotFather>
TELEGRAM_CHAT_ID=<chat_id_atau_group_id>
PORT=5000
ANOMALY_CONSECUTIVE_COUNT=2
```

### Jalankan Server

```bash
cd server
python app.py
```

Output yang muncul saat startup:

```
Loading models...
Sensor threshold : 0.384800
Audio threshold  : 0.612700
All models loaded.
BeeWatch server running on port 5000
```

### Cek Status Server

```bash
curl http://localhost:5000/health
```

---

## API Endpoint

### POST /upload/sensor

Menerima data sensor dari ESP32 Sensor Node.

**Request body (JSON):**
```json
{
  "temperature": 35.5,
  "humidity": 62.3,
  "pressure": 1008.1
}
```

**Response:**
```json
{
  "status": "received",
  "sensor_error": 0.0023,
  "sensor_anomaly": false,
  "waiting_audio": true
}
```

Jika `waiting_audio` bernilai `true`, berarti server sedang menunggu data audio untuk menghitung combined score.

---

### POST /upload/audio

Menerima file audio WAV dari ESP32 Audio Node.

**Request:** `multipart/form-data` dengan field `audio` berisi file `.wav`

```bash
curl -X POST http://localhost:5000/upload/audio \
  -F "audio=@recording.wav"
```

**Response:**
```json
{
  "status": "received",
  "audio_score": 0.42,
  "audio_anomaly": false,
  "severity": "normal",
  "waiting_sensor": false
}
```

Nilai `severity` yang mungkin: `normal`, `warning`, atau `critical`.

---

### GET /health

Memeriksa status server dan parameter model yang sedang aktif.

**Response:**
```json
{
  "status": "ok",
  "sensor_threshold": 0.384800,
  "audio_threshold": 0.612700,
  "combined_threshold": 0.5,
  "w_sensor": 0.5,
  "w_audio": 0.5,
  "time_window_sec": 300
}
```

---

## Training Model

### Sensor Autoencoder (Lokal)

1. Unduh dataset dari Kaggle: [annajyang/beehive-sounds](https://www.kaggle.com/datasets/annajyang/beehive-sounds)
2. Letakkan `all_data_updated.csv` di dalam folder `data/sensor_data/`
3. Jalankan `sensor_training.ipynb` dari sel pertama hingga terakhir
4. Salin artefak yang dihasilkan ke folder `models/`:
   - `sensor_autoencoder.keras`
   - `sensor_scaler.pkl`
   - `sensor_threshold.npy`

### Audio Autoencoder (Kaggle Notebook dengan GPU T4)

Proses ekstraksi fitur dari 7.100 file WAV membutuhkan waktu yang cukup lama. Sangat disarankan untuk menjalankan notebook ini di Kaggle dengan akselerasi GPU.

1. Buka `audio_training.ipynb` di Kaggle
2. Tambahkan dataset `annajyang/beehive-sounds` melalui menu **+ Add Data**
3. Opsional: tambahkan dataset cache `beewatch-cache` untuk melewati proses ekstraksi fitur yang sudah pernah dilakukan sebelumnya
4. Jalankan semua sel dengan **Run All**
5. Unduh artefak dari panel Output Kaggle, lalu salin ke folder `models/`:
   - `audio_autoencoder.keras`
   - `audio_scaler.pkl`
   - `model_config.pkl`
   - `thresholds.pkl`

> Setelah proses ekstraksi fitur selesai pertama kali, unduh file `audio_features_cache.pkl` dari output Kaggle dan upload sebagai dataset baru bernama `beewatch-cache`. Pada run berikutnya, notebook akan otomatis memuatnya dan melewati proses ekstraksi yang memakan waktu lama.

---

## Dependensi

| Paket | Versi |
|---|---|
| tensorflow | 2.18.0 |
| scikit-learn | 1.5.2 |
| numpy | 1.26.4 |
| pandas | 2.2.2 |
| librosa | 0.10.2 |
| flask | 3.0.3 |
| python-telegram-bot | 20.7 |
| python-dotenv | 1.0.1 |
| matplotlib | 3.9.2 |
| seaborn | 0.13.2 |

---

## Lisensi

Proyek ini dilisensikan di bawah ketentuan yang tercantum di file [LICENSE](LICENSE).
