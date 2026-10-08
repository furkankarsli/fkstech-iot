from flask import Flask, render_template, jsonify, request
from flask_cors import CORS
import sqlite3
import json
import os
import threading
import time
import random
from datetime import datetime

app = Flask(__name__)
CORS(app)

from zk.web import zk_bp, baslat as zk_baslat  # /zk: yemek kartı rakip takibi
app.register_blueprint(zk_bp)
zk_baslat()

system_state = {
    "istasyon_1": {
        "gaz": 120,
        "alev": 0,
        "sicaklik_C": 24.5,
        "nem_Yuzde": 45.0,
        "gaz_esik": 400
    },
    "istasyon_2": {
        "akim_mA": 420.0,
        "guc_W": 92.4,
        "kapi": "kapali",
        "guvenlik_aktif": True,
        "rele": "acik"
    },
    "led_status": ["kapali", "kapali", "kapali"],
    "merkezi_alarm": "pasif",
    "oled_mod": "alfabe",
    "oled_mesaj": "Hazir"
}

API_KEY = "tasarim_projesi_secret_key"
DB_FILE = "database.db"

def init_db():
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
    cursor.execute("PRAGMA journal_mode=WAL;")
    cursor.execute("PRAGMA synchronous=NORMAL;")
    cursor.execute("""
    CREATE TABLE IF NOT EXISTS sensor_logs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        timestamp TEXT,
        istasyon_id INTEGER,
        data TEXT
    )
    """)
    cursor.execute("""
    CREATE TABLE IF NOT EXISTS system_settings (
        key TEXT PRIMARY KEY,
        value TEXT
    )
    """)
    conn.commit()
    conn.close()

def save_state_to_db():
    try:
        conn = sqlite3.connect(DB_FILE)
        cursor = conn.cursor()
        cursor.execute("INSERT OR REPLACE INTO system_settings (key, value) VALUES (?, ?)",
                       ("state", json.dumps(system_state)))
        conn.commit()
        conn.close()
    except Exception as e:
        print(f"Error saving state to DB: {e}")

def load_state_from_db():
    global system_state
    if not os.path.exists(DB_FILE):
        return
    try:
        conn = sqlite3.connect(DB_FILE)
        cursor = conn.cursor()
        cursor.execute("SELECT value FROM system_settings WHERE key = ?", ("state",))
        row = cursor.fetchone()
        if row:
            saved_state = json.loads(row[0])
            for k, v in saved_state.items():
                if k in system_state:
                    if isinstance(system_state[k], dict) and isinstance(v, dict):
                        system_state[k].update(v)
                    else:
                        system_state[k] = v
        conn.close()
    except Exception as e:
        print(f"Error loading state from DB: {e}")

def log_sensor_data(istasyon_id, data):
    try:
        conn = sqlite3.connect(DB_FILE)
        cursor = conn.cursor()
        timestamp = datetime.now().strftime('%Y-%m-%d %H:%M:%S')
        cursor.execute("INSERT INTO sensor_logs (timestamp, istasyon_id, data) VALUES (?, ?, ?)",
                       (timestamp, istasyon_id, json.dumps(data)))
        conn.commit()
        conn.close()
    except Exception as e:
        print(f"Error logging sensor data to DB: {e}")

init_db()
load_state_from_db()

def simulate_sensor_drift():
    def run_sim():
        possible_metrics = ["sicaklik", "nem", "gaz", "akim"]
        while True:
            time.sleep(2.0)
            try:
                # Her 2 saniyede rastgele 1 veya 2 metrik seçilip çok hafif değiştirilir
                selected_metrics = random.sample(possible_metrics, k=random.randint(1, 2))

                if "sicaklik" in selected_metrics:
                    curr_t = system_state["istasyon_1"]["sicaklik_C"]
                    delta_t = random.choice([-0.1, 0.1])
                    system_state["istasyon_1"]["sicaklik_C"] = round(max(23.5, min(25.8, curr_t + delta_t)), 1)

                if "nem" in selected_metrics:
                    curr_h = system_state["istasyon_1"]["nem_Yuzde"]
                    delta_h = random.choice([-0.2, -0.1, 0.1, 0.2])
                    system_state["istasyon_1"]["nem_Yuzde"] = round(max(43.0, min(48.0, curr_h + delta_h)), 1)

                if "gaz" in selected_metrics:
                    curr_g = system_state["istasyon_1"]["gaz"]
                    delta_g = random.choice([-2, -1, 1, 2])
                    system_state["istasyon_1"]["gaz"] = max(112, min(135, curr_g + delta_g))

                if "akim" in selected_metrics:
                    rele_on = system_state["istasyon_2"]["rele"] == "acik"
                    curr_i = system_state["istasyon_2"]["akim_mA"]
                    if rele_on:
                        if curr_i < 100.0: curr_i = 430.0
                        delta_i = random.uniform(-2.5, 2.5)
                        new_i = round(max(410.0, min(460.0, curr_i + delta_i)), 1)
                    else:
                        if curr_i > 100.0: curr_i = 22.0
                        delta_i = random.uniform(-0.5, 0.5)
                        new_i = round(max(18.0, min(26.0, curr_i + delta_i)), 1)

                    system_state["istasyon_2"]["akim_mA"] = new_i
                    system_state["istasyon_2"]["guc_W"] = round((220.0 * new_i) / 1000.0, 2)

                alarm_denetimi()
            except Exception as e:
                print(f"Simulation error: {e}")

    thread = threading.Thread(target=run_sim, daemon=True)
    thread.start()

def alarm_denetimi():
    alarm = False
    if system_state["istasyon_1"]["gaz"] >= system_state["istasyon_1"]["gaz_esik"]:
        alarm = True
    if system_state["istasyon_1"]["alev"] == 1:
        alarm = True
    if system_state["istasyon_2"]["guvenlik_aktif"] and system_state["istasyon_2"]["kapi"] == "acik":
        alarm = True
    system_state["merkezi_alarm"] = "aktif" if alarm else "pasif"

simulate_sensor_drift()

@app.route('/')
def home():
    return render_template('index.html')

@app.route('/pico/data', methods=['POST'])
def receive_data():
    req_api_key = request.headers.get('X-API-Key')
    if req_api_key != API_KEY:
        return jsonify({"status": "hata", "mesaj": "Yetkisiz istek (Gecersiz API Key)"}), 401

    data = request.get_json()
    if not data:
        return jsonify({"status": "hata", "mesaj": "JSON verisi bulunamadi"}), 400

    if isinstance(data, list):
        updates = data
    else:
        updates = [data]

    for update in updates:
        istasyon_id = update.get("istasyon_id")

        if istasyon_id == 1:
            system_state["istasyon_1"]["gaz"] = int(update.get("gaz", system_state["istasyon_1"]["gaz"]))
            system_state["istasyon_1"]["alev"] = int(update.get("alev", system_state["istasyon_1"]["alev"]))
            system_state["istasyon_1"]["sicaklik_C"] = float(update.get("sicaklik_C", system_state["istasyon_1"]["sicaklik_C"]))
            system_state["istasyon_1"]["nem_Yuzde"] = float(update.get("nem_Yuzde", system_state["istasyon_1"]["nem_Yuzde"]))

            log_sensor_data(1, {
                "gaz": system_state["istasyon_1"]["gaz"],
                "alev": system_state["istasyon_1"]["alev"],
                "sicaklik_C": system_state["istasyon_1"]["sicaklik_C"],
                "nem_Yuzde": system_state["istasyon_1"]["nem_Yuzde"]
            })

        elif istasyon_id == 2:
            system_state["istasyon_2"]["akim_mA"] = float(update.get("akim_mA", system_state["istasyon_2"]["akim_mA"]))
            system_state["istasyon_2"]["guc_W"] = round((220.0 * system_state["istasyon_2"]["akim_mA"]) / 1000.0, 2)
            system_state["istasyon_2"]["kapi"] = update.get("kapi", system_state["istasyon_2"]["kapi"])

            log_sensor_data(2, {
                "akim_mA": system_state["istasyon_2"]["akim_mA"],
                "guc_W": system_state["istasyon_2"]["guc_W"],
                "kapi": system_state["istasyon_2"]["kapi"]
            })

    alarm_denetimi()
    save_state_to_db()
    return jsonify({"status": "basarili", "merkezi_alarm": system_state["merkezi_alarm"]}), 200

@app.route('/status', methods=['GET'])
def get_status():
    req_api_key = request.headers.get('X-API-Key')
    if req_api_key != API_KEY:
         return jsonify({"status": "hata", "mesaj": "Yetkisiz istek"}), 401

    return jsonify({
        "rele": system_state["istasyon_2"]["rele"],
        "ledler": [x.capitalize() for x in system_state["led_status"]],
        "oled_mod": system_state["oled_mod"],
        "oled_mesaj": system_state["oled_mesaj"],
        "guvenlik_aktif": system_state["istasyon_2"]["guvenlik_aktif"]
    }), 200

@app.route('/api/data', methods=['GET'])
def get_all_data():
    return jsonify(system_state), 200

@app.route('/api/control', methods=['POST'])
def control_actuator():
    data = request.get_json()
    if not data:
        return jsonify({"status": "hata", "mesaj": "Gecersiz istek"}), 400

    target = data.get("target")
    value = data.get("value")

    if target == "rele":
        system_state["istasyon_2"]["rele"] = "acik" if value == "acik" else "kapali"
    elif target == "led":
        idx = int(data.get("index", 0))
        if 0 <= idx < 3:
            system_state["led_status"][idx] = "acik" if value == "acik" else "kapali"
    elif target == "guvenlik":
        system_state["istasyon_2"]["guvenlik_aktif"] = bool(value)
    elif target == "oled_mod":
        if value in ["alfabe", "kapali", "mesaj"]:
            system_state["oled_mod"] = value
    elif target == "oled_mesaj":
        system_state["oled_mesaj"] = str(value)[:16]
    elif target == "alarm_sifirla":
        system_state["istasyon_1"]["gaz"] = 120
        system_state["istasyon_1"]["alev"] = 0
        system_state["istasyon_2"]["kapi"] = "kapali"
        system_state["merkezi_alarm"] = "pasif"

    alarm_denetimi()
    save_state_to_db()
    return jsonify({"status": "basarili", "state": system_state}), 200

if __name__ == '__main__':
    print("--------------------------------------------------")
    print("Akilli Ev Projesi Yerel Sunucu Baslatiliyor...")
    print("Adres: http://127.0.0.1:5000")
    print("--------------------------------------------------")
    app.run(host='0.0.0.0', port=5000, debug=False, threaded=True)
