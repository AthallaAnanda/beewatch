import sqlite3
from datetime import datetime
from pathlib import Path

DB_PATH = Path(__file__).parent.parent / 'data' / 'beewatch.db'

def init_db():
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    conn.execute('''
        CREATE TABLE IF NOT EXISTS readings (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp    TEXT    NOT NULL,
            temperature  REAL,
            humidity     REAL,
            pressure     REAL,
            sensor_error REAL,
            is_anomaly   INTEGER
        )
    ''')
    conn.commit()
    conn.close()

def log_reading(data: dict):
    conn = sqlite3.connect(DB_PATH)
    conn.execute('''
        INSERT INTO readings
            (timestamp, temperature, humidity, pressure, sensor_error, is_anomaly)
        VALUES (?, ?, ?, ?, ?, ?)
    ''', (
        datetime.now().isoformat(),
        data.get('temperature'),
        data.get('humidity'),
        data.get('pressure'),
        data.get('sensor_error'),
        1 if data.get('is_anomaly') else 0
    ))
    conn.commit()
    conn.close()