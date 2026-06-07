import asyncio
from datetime import datetime
from telegram import Bot

class TelegramNotifier:
    def __init__(self, token: str, chat_id: str):
        self.bot     = Bot(token=token)
        self.chat_id = chat_id

    def _run(self, coro):
        try:
            loop = asyncio.get_event_loop()
            if loop.is_closed():
                loop = asyncio.new_event_loop()
                asyncio.set_event_loop(loop)
            return loop.run_until_complete(coro)
        except RuntimeError:
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)
            return loop.run_until_complete(coro)

    def send_combined_report(self, data: dict):
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        status    = "⚠️ ANOMALI" if data.get('is_anomaly') else "✅ NORMAL"
        msg = (
            f"📊 *[BeeWatch] Laporan Rutin*\n"
            f"🕐 Waktu          : `{timestamp}`\n"
            f"🌡️ Suhu           : `{data.get('temperature', 0):.1f} °C`\n"
            f"💧 Kelembaban     : `{data.get('humidity', 0):.1f} %RH`\n"
            f"📈 Tekanan        : `{data.get('pressure', 0):.1f} hPa`\n"
            f"🌿 Sensor Score   : `{data.get('sensor_score', 0):.4f}`\n"
            f"🔊 Audio Score    : `{data.get('audio_score', 0):.4f}`\n"
            f"⚖️ Combined Score : `{data.get('combined_score', 0):.4f}`\n"
            f"📌 Status         : *{status}*"
        )
        self._run(self.bot.send_message(
            chat_id=self.chat_id, text=msg, parse_mode='Markdown'
        ))

    def send_emergency_alert(self, data: dict):
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        msg = (
            f"🚨 *[BeeWatch] PERINGATAN ANOMALI*\n"
            f"🕐 Waktu          : `{timestamp}`\n"
            f"🌡️ Suhu            : `{data.get('temperature', 0):.1f} °C`\n"
            f"💧 Kelembaban      : `{data.get('humidity', 0):.1f} %RH`\n"
            f"📈 Tekanan        : `{data.get('pressure', 0):.1f} hPa`\n"
            f"🌿 Sensor Score   : `{data.get('sensor_score', 0):.4f}`\n"
            f"🔊 Audio Score    : `{data.get('audio_score', 0):.4f}`\n"
            f"⚖️ Combined Score : `{data.get('combined_score', 0):.4f}`\n"
            f"❗ Status           : *ANOMALI TERDETEKSI*\n"
            f"🔍 Rekomendasi    : _Segera periksa kondisi sarang Anda._"
        )
        self._run(self.bot.send_message(
            chat_id=self.chat_id, text=msg, parse_mode='Markdown'
        ))