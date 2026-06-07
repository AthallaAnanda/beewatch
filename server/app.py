import os
import time
from flask import Flask, request, jsonify
from dotenv import load_dotenv
from pathlib import Path

from inference import BeeWatchInference
from notifier  import TelegramNotifier
from database  import init_db, log_reading

load_dotenv(Path(__file__).parent / '.env')

app       = Flask(__name__)
inference = BeeWatchInference()
notifier  = TelegramNotifier(
    token  =os.getenv('TELEGRAM_BOT_TOKEN'),
    chat_id=os.getenv('TELEGRAM_CHAT_ID')
)
init_db()

# ── Combined score config ────────────────────────────
W_SENSOR           = 0.5
W_AUDIO            = 0.5
COMBINED_THRESHOLD = 0.5
TIME_WINDOW_SEC    = 300   # toleransi selisih waktu sensor vs audio (5 menit)
STREAK_THRESHOLD   = int(os.getenv('ANOMALY_CONSECUTIVE_COUNT', 2))

latest_sensor = None  # {'score': float, 'data': dict, 'time': float}
latest_audio  = None  # {'score': float, 'result': dict, 'time': float}
anomaly_streak = 0
# ────────────────────────────────────────────────────


def try_compute_combined():
    global latest_sensor, latest_audio, anomaly_streak

    if latest_sensor is None or latest_audio is None:
        return

    time_diff = abs(latest_sensor['time'] - latest_audio['time'])
    if time_diff > TIME_WINDOW_SEC:
        return

    # Normalisasi sensor error ke 0-1
    sensor_raw   = latest_sensor['score']
    sensor_score = min(sensor_raw / (inference.sensor_threshold * 2), 1.0)

    # Audio score sudah 0-1
    audio_score = latest_audio['score']

    # Combined 50:50
    combined   = W_SENSOR * sensor_score + W_AUDIO * audio_score
    is_anomaly = combined > COMBINED_THRESHOLD

    report = {
        **latest_sensor['data'],
        'sensor_score'  : round(sensor_score, 4),
        'audio_score'   : round(audio_score, 4),
        'combined_score': round(combined, 4),
        'is_anomaly'    : is_anomaly,
    }

    log_reading({
        **latest_sensor['data'],
        'sensor_error': sensor_raw,
        'is_anomaly'  : is_anomaly,
    })

    notifier.send_combined_report(report)

    if is_anomaly:
        anomaly_streak += 1
        if anomaly_streak >= STREAK_THRESHOLD:
            notifier.send_emergency_alert(report)
            anomaly_streak = 0
    else:
        anomaly_streak = 0

    # Reset setelah combined dihitung
    latest_sensor = None
    latest_audio  = None


@app.route('/upload/sensor', methods=['POST'])
def upload_sensor():
    global latest_sensor

    data = request.get_json()
    if not data:
        return jsonify({'error': 'No JSON data'}), 400

    for field in ['temperature', 'humidity', 'pressure']:
        if field not in data:
            return jsonify({'error': f'Missing field: {field}'}), 400

    sensor_data = {
        'temperature': float(data['temperature']),
        'humidity'   : float(data['humidity']),
        'pressure'   : float(data['pressure']),
    }

    try:
        result = inference.compute_sensor_score(sensor_data)

        latest_sensor = {
            'score': result['sensor_error'],
            'data' : sensor_data,
            'time' : time.time()
        }

        try_compute_combined()

        return jsonify({
            'status'        : 'received',
            'sensor_error'  : result['sensor_error'],
            'sensor_anomaly': result['sensor_anomaly'],
            'waiting_audio' : latest_sensor is not None
        }), 200

    except Exception as e:
        return jsonify({'error': str(e)}), 500


@app.route('/upload/audio', methods=['POST'])
def upload_audio():
    global latest_audio

    if 'audio' not in request.files:
        return jsonify({'error': 'No audio file'}), 400

    wav_path = '/tmp/beewatch_audio.wav'
    request.files['audio'].save(wav_path)

    try:
        result = inference.compute_audio_score(wav_path)

        latest_audio = {
            'score' : result['audio_score'],
            'result': result,
            'time'  : time.time()
        }

        try_compute_combined()

        return jsonify({
            'status'        : 'received',
            'audio_score'   : result['audio_score'],
            'audio_anomaly' : result['is_anomaly'],
            'severity'      : result['severity'],
            'waiting_sensor': latest_audio is not None
        }), 200

    except Exception as e:
        return jsonify({'error': str(e)}), 500


@app.route('/health', methods=['GET'])
def health():
    return jsonify({
        'status'            : 'ok',
        'sensor_threshold'  : inference.sensor_threshold,
        'audio_threshold'   : inference.audio_thr['audio_threshold_pct95'],
        'combined_threshold': COMBINED_THRESHOLD,
        'w_sensor'          : W_SENSOR,
        'w_audio'           : W_AUDIO,
        'time_window_sec'   : TIME_WINDOW_SEC,
    }), 200


if __name__ == '__main__':
    port = int(os.getenv('PORT', 5000))
    print(f"BeeWatch server running on port {port}")
    app.run(host='0.0.0.0', port=port, debug=False)