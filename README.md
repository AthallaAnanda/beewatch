# 🐝 BeeWatch — Smart Hive Integrated Monitoring System

BeeWatch adalah sistem pemantauan sarang lebah cerdas berbasis **IoT + Machine Learning** yang mendeteksi anomali kondisi sarang secara *real-time* menggunakan dua modalitas: **sensor lingkungan** (suhu, kelembaban, tekanan) dan **audio akustik** dari dalam sarang.

---

## Arsitektur Sistem

```
ESP32 (Sensor)          ESP32 (Audio)
  │ suhu, kelembaban,     │ rekaman WAV
  │ tekanan               │ (INMP441 mic)
  └──────────┬────────────┘
             │  HTTP POST
             ▼
      Flask Server (app.py)
       ├── /upload/sensor ──► Sensor Autoencoder
       ├── /upload/audio  ──► Audio Autoencoder
       └── Combined Score (50% sensor + 50% audio)
             │
             ├── SQLite (log pembacaan)
             └── Telegram Bot (notifikasi)
```

### Alur Deteksi Anomali

1. ESP32 sensor mengirim data suhu/kelembaban/tekanan ke `/upload/sensor`
2. ESP32 audio mengirim file WAV ke `/upload/audio`
3. Server menggabungkan skor dari kedua model dalam jendela waktu **5 menit**
4. Jika `combined_score > 0.5` → status **ANOMALI**
5. Jika anomali terjadi berturut-turut (`ANOMALY_CONSECUTIVE_COUNT` kali) → **peringatan darurat** dikirim via Telegram

---

## Model Machine Learning

Kedua model dilatih secara **unsupervised** menggunakan pendekatan *autoencoder*. Model mempelajari pola normal sarang; reconstruction error yang tinggi mengindikasikan kondisi menyimpang (anomali).

### Dataset

- **Sumber:** [Kaggle — Beehive Sounds (annajyang)](https://www.kaggle.com/datasets/annajyang/beehive-sounds)
- **Isi:** 7.100 file WAV (rekaman 4 koloni lebah madu Eropa) + `all_data_updated.csv` (data lingkungan & status ratu)
- **Periode:** Rekaman setiap 15 menit dari node ESP32

### Sensor Autoencoder

| Komponen | Detail |
|---|---|
| Input | 3 fitur: `hive temp`, `hive humidity`, `hive pressure` |
| Arsitektur | 3 → 16 → **8 (bottleneck)** → 16 → 3 |
| Framework | TensorFlow / Keras |
| Scaler | `StandardScaler` (fit hanya di data train) |
| Threshold | Persentil 95 reconstruction error (MSE) |
| Artefak | `sensor_autoencoder.keras`, `sensor_scaler.pkl`, `sensor_threshold.npy` |

### Audio Autoencoder

| Komponen | Detail |
|---|---|
| Input | 72 fitur akustik per file WAV |
| Fitur | MFCC (13×2), Mel-Spectrogram (20×2), Spectral Centroid, Rolloff, ZCR |
| Arsitektur | 72 → 64 → **32 (bottleneck)** → 64 → 72 |
| Framework | TensorFlow / Keras |
| Scaler | `StandardScaler` |
| Threshold | Persentil 95 reconstruction error (P95) |
| Artefak | `audio_autoencoder.keras`, `audio_scaler.pkl`, `model_config.pkl`, `thresholds.pkl` |

#### Ekstraksi Fitur Audio (72 dimensi)

| Fitur | Dimensi |
|---|---|
| MFCC mean + std (13 koefisien) | 26 |
| Mel-Spectrogram mean + std (20 band) | 40 |
| Spectral Centroid mean + std | 2 |
| Spectral Rolloff mean + std | 2 |
| Zero Crossing Rate mean + std | 2 |
| **Total** | **72** |

### Penggabungan Skor (Combined Score)

```
combined_score = 0.5 × sensor_score + 0.5 × audio_score

is_anomaly = combined_score > 0.5
```

- `sensor_score`: reconstruction error sensor dinormalisasi ke `[0, 1]` relatif terhadap threshold
- `audio_score`: Z-score dari reconstruction error audio → dikliping ke `[0, 1]`

---

## Struktur Proyek

```
beewatch/
├── server/
│   ├── app.py          # Flask server, endpoint HTTP, logika combined score
│   ├── inference.py    # Kelas BeeWatchInference (load model + hitung skor)
│   ├── database.py     # Inisialisasi & logging SQLite
│   ├── notifier.py     # Telegram bot (laporan rutin & peringatan darurat)
│   └── .env            # Konfigurasi (token Telegram, port, dll.)
├── training/
│   ├── sensor_training.ipynb   # Notebook training Sensor Autoencoder
│   ├── audio_training.ipynb    # Notebook training Audio Autoencoder (Kaggle GPU)
│   └── *.png                   # Grafik hasil pelatihan & evaluasi
├── models/             # Artefak model (diisi setelah training)
│   ├── sensor_autoencoder.keras
│   ├── sensor_scaler.pkl
│   ├── sensor_threshold.npy
│   ├── audio_autoencoder.keras
│   ├── audio_scaler.pkl
│   ├── model_config.pkl
│   └── thresholds.pkl
├── data/               # Database SQLite (dibuat otomatis saat server dijalankan)
│   └── beewatch.db
└── requirements.txt
```

---

## Instalasi & Menjalankan Server

### 1. Prasyarat

- Python 3.10+
- Model sudah tersedia di folder `models/` (hasil training notebook)

### 2. Clone & Install Dependensi

```bash
git clone https://github.com/<username>/beewatch.git
cd beewatch
python -m venv venv

# Windows
venv\Scripts\activate
# Linux/macOS
source venv/bin/activate

pip install -r requirements.txt
```

### 3. Konfigurasi Environment

Buat file `server/.env`:

```env
TELEGRAM_BOT_TOKEN=<token_bot_telegram_anda>
TELEGRAM_CHAT_ID=<chat_id_tujuan>
PORT=5000
ANOMALY_CONSECUTIVE_COUNT=2   # jumlah anomali berturut-turut sebelum alert darurat
```

### 4. Jalankan Server

```bash
cd server
python app.py
```

Server berjalan di `http://0.0.0.0:5000`.

---

## API Endpoint

### `POST /upload/sensor`

Menerima data sensor dari ESP32.

**Request Body (JSON):**
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

---

### `POST /upload/audio`

Menerima file audio WAV dari ESP32.

**Request:** `multipart/form-data` dengan field `audio` berisi file `.wav`

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

Nilai `severity`: `normal` | `warning` | `critical`

---

### `GET /health`

Memeriksa status server dan parameter model yang aktif.

**Response:**
```json
{
  "status": "ok",
  "sensor_threshold": 0.004521,
  "audio_threshold": 0.6873,
  "combined_threshold": 0.5,
  "w_sensor": 0.5,
  "w_audio": 0.5,
  "time_window_sec": 300
}
```

---

## Notifikasi Telegram

Server mengirim dua jenis pesan ke Telegram:

**📊 Laporan Rutin** — dikirim setiap combined score berhasil dihitung:
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

**🚨 Peringatan Darurat** — dikirim setelah anomali terdeteksi N kali berturut-turut:
```
🚨 [BeeWatch] PERINGATAN ANOMALI
...
❗ Status           : ANOMALI TERDETEKSI
🔍 Rekomendasi    : Segera periksa kondisi sarang Anda.
```

---

## Training Model

### Sensor Autoencoder

Jalankan `training/sensor_training.ipynb` secara lokal. Dataset `all_data_updated.csv` diunduh dari Kaggle.

Salin artefak output ke folder `models/`:
- `sensor_autoencoder.keras`
- `sensor_scaler.pkl`
- `sensor_threshold.npy`

### Audio Autoencoder

Jalankan `training/audio_training.ipynb` di **Kaggle Notebook** (disarankan menggunakan GPU T4).

1. Tambahkan dataset `annajyang/beehive-sounds` via **+ Add Data**
2. Opsional: tambahkan dataset cache `beewatch-cache` untuk skip ekstraksi fitur ulang
3. **Run All** dari atas ke bawah
4. Unduh artefak dari panel *Output* Kaggle dan salin ke folder `models/`:
   - `audio_autoencoder.keras`
   - `audio_scaler.pkl`
   - `model_config.pkl`
   - `thresholds.pkl`

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
