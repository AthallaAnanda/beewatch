import numpy as np
import pickle
import joblib
import librosa
import tensorflow as tf
from pathlib import Path

MODEL_DIR = Path(__file__).parent.parent / 'models'

class BeeWatchInference:
    def __init__(self):
        print("Loading models...")

        # --- Sensor model ---
        self.sensor_model = tf.keras.models.load_model(
            MODEL_DIR / 'sensor_autoencoder.keras'
        )
        with open(MODEL_DIR / 'sensor_scaler.pkl', 'rb') as f:
            self.sensor_scaler = pickle.load(f)
        self.sensor_threshold = float(
            np.load(MODEL_DIR / 'sensor_threshold.npy')
        )
        print(f"Sensor threshold : {self.sensor_threshold:.6f}")

        # --- Audio model ---
        self.audio_model  = tf.keras.models.load_model(
            MODEL_DIR / 'audio_autoencoder.keras'
        )
        self.audio_scaler = joblib.load(MODEL_DIR / 'audio_scaler.pkl')
        self.audio_cfg    = joblib.load(MODEL_DIR / 'model_config.pkl')
        self.audio_thr    = joblib.load(MODEL_DIR / 'thresholds.pkl')

        # Warmup audio model
        _ = self.audio_model.predict(
            np.zeros((1, self.audio_cfg['audio_input_dim'])), verbose=0
        )
        print(f"Audio threshold  : {self.audio_thr['audio_threshold_pct95']:.6f}")
        print("All models loaded.")

    def compute_sensor_score(self, sensor_data: dict) -> dict:
        features = np.array([[
            sensor_data['temperature'],
            sensor_data['humidity'],
            sensor_data['pressure'],
        ]], dtype=np.float32)
        feat_norm      = self.sensor_scaler.transform(features)
        reconstruction = self.sensor_model.predict(feat_norm, verbose=0)
        error = float(np.mean((feat_norm - reconstruction) ** 2))
        return {
            'sensor_error'    : error,
            'sensor_threshold': self.sensor_threshold,
            'sensor_anomaly'  : error > self.sensor_threshold
        }

    def _extract_audio_features(self, seg, sr):
        cfg    = self.audio_cfg
        n_mfcc = cfg['n_mfcc']; n_mels = cfg['n_mels']
        n_fft  = cfg['n_fft'];  hop    = cfg['hop_length']
        mfcc   = librosa.feature.mfcc(y=seg, sr=sr, n_mfcc=n_mfcc, n_fft=n_fft, hop_length=hop)
        mel    = librosa.feature.melspectrogram(y=seg, sr=sr, n_fft=n_fft, hop_length=hop, n_mels=128)
        mel_db = librosa.power_to_db(mel, ref=np.max)
        sc     = librosa.feature.spectral_centroid(y=seg, sr=sr, n_fft=n_fft, hop_length=hop)
        srf    = librosa.feature.spectral_rolloff(y=seg, sr=sr, n_fft=n_fft, hop_length=hop)
        zcr    = librosa.feature.zero_crossing_rate(y=seg, hop_length=hop)
        return np.concatenate([
            np.mean(mfcc, axis=1), np.std(mfcc, axis=1),
            np.mean(mel_db, axis=1)[:n_mels], np.std(mel_db, axis=1)[:n_mels],
            [np.mean(sc), np.std(sc), np.mean(srf), np.std(srf), np.mean(zcr), np.std(zcr)]
        ])

    def compute_audio_score(self, wav_path: str) -> dict:
        cfg = self.audio_cfg
        sr  = cfg['sr']
        sl  = int(cfg['segment_duration'] * sr)

        try:
            audio, _ = librosa.load(wav_path, sr=sr, mono=True)
        except Exception as e:
            return {'audio_score': 0.0, 'is_anomaly': False,
                    'severity': 'normal', 'error': str(e)}

        n_segs = len(audio) // sl
        if n_segs == 0:
            return {'audio_score': 0.0, 'is_anomaly': False,
                    'severity': 'normal', 'n_segments': 0}

        feats       = [self._extract_audio_features(audio[i*sl:(i+1)*sl], sr)
                       for i in range(n_segs)]
        feat_scaled = self.audio_scaler.transform(
            np.mean(feats, axis=0).reshape(1, -1)
        )
        recon   = self.audio_model.predict(feat_scaled, verbose=0)
        raw_err = float(np.mean((feat_scaled - recon) ** 2))

        print(f"[DEBUG] n_segs={n_segs}, raw_err={raw_err:.6f}")
        print(f"[DEBUG] feat_scaled mean={feat_scaled.mean():.4f} std={feat_scaled.std():.4f} min={feat_scaled.min():.4f} max={feat_scaled.max():.4f}")
        print(f"[DEBUG] thresholds keys: {list(self.audio_thr.keys())}")
        
        if 'audio_raw_p95' in self.audio_thr:
            # Thresholds.pkl sudah punya raw stats → pakai ini
            raw_p95  = self.audio_thr['audio_raw_p95']
            raw_mean = self.audio_thr['audio_raw_mean']
            score = float(np.clip(
                (raw_err - raw_mean) / ((raw_p95 - raw_mean) * 2 + 1e-8),
                0.0, 1.0
            ))
            is_anomaly = raw_err > raw_p95
        else:
            thr_val    = self.audio_thr.get('audio_threshold_pct95', 0.35)
            err_mean   = self.audio_thr.get('audio_mean', 0.0)
            err_std    = self.audio_thr.get('audio_std', 1.0)
            upper      = err_mean + 3 * err_std
            score      = float(np.clip(
                (raw_err - err_mean) / (upper - err_mean + 1e-8),
                0.0, 1.0
            ))
            is_anomaly = raw_err > thr_val

        severity = 'normal'
        if is_anomaly:
            severity = 'critical' if score > 0.75 else 'warning'
            
        return {
            'audio_score' : score,
            'is_anomaly'  : is_anomaly,
            'severity'    : severity,
            'raw_error'   : raw_err,
            'n_segments'  : n_segs
        }
