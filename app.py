from flask import Flask, render_template, jsonify, request
from flask_cors import CORS
import time
import sqlite3
import json
import os
from datetime import datetime

app = Flask(__name__)
CORS(app)

# Global sistem durumu (2 İstasyonlu Model)
system_state = {
    # İstasyon 1: Ev Çevre ve Gaz/Yangın Güvenliği
    "istasyon_1": {
        "gaz": 120,          # MQ-9 ADC/PPM değeri
        "alev": 0,           # 0: alev yok, 1: alev var
        "sicaklik_C": 24.5,
        "nem_Yuzde": 45.0,
        "gaz_esik": 400
    },
    # İstasyon 2: Giriş Güvenliği ve Enerji İzleme/Kontrol
    "istasyon_2": {
        "akim_mA": 0.0,
        "guc_W": 0.0,
        "kapi": "kapali",    # "acik" veya "kapali"
        "guvenlik_aktif": True,
        "rele": "kapali"     # "acik" veya "kapali"
    },
    # Genel Aktüatör Durumları (LED'ler)
    "led_status": ["kapali", "kapali", "kapali"], # LED1, LED2, LED3
    "merkezi_alarm": "pasif",
    
    # OLED Ekran Durumları (Web Kontrolü)
    "oled_mod": "alfabe",    # "alfabe", "kapali", "mesaj"
    "oled_mesaj": "Hazir"
}

# Güvenlik için API Anahtarı
API_KEY = "tasarim_projesi_secret_key"

DB_FILE = "database.db"

def init_db():
    conn = sqlite3.connect(DB_FILE)
    cursor = conn.cursor()
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

# Veritabanını ilklendir ve kayıtlı durumu yükle
init_db()
load_state_from_db()

def alarm_denetimi():
    """Sensör eşik değerlerine göre alarm durumunu günceller."""
    alarm = False
    
    # 1. İstasyon 1: Gaz Kaçağı
    if system_state["istasyon_1"]["gaz"] >= system_state["istasyon_1"]["gaz_esik"]:
        alarm = True
        
    # 1. İstasyon 1: Yangın/Alev
    if system_state["istasyon_1"]["alev"] == 1:
        alarm = True
        
    # 2. İstasyon 2: İzinsiz Giriş (Güvenlik aktifken kapı açılırsa)
    if system_state["istasyon_2"]["guvenlik_aktif"] and system_state["istasyon_2"]["kapi"] == "acik":
        alarm = True
        
    system_state["merkezi_alarm"] = "aktif" if alarm else "pasif"

@app.route('/')
def home():
    """Ana Dashboard Arayüzünü sunar."""
    return render_template('index.html')

@app.route('/pico/data', methods=['POST'])
def receive_data():
    """
    Raporla uyumlu ana veri alma ucu. 
    Toplu listeleri (Array/List) veya tekli güncellemeleri kabul eder.
    """
    # X-API-Key doğrulaması
    req_api_key = request.headers.get('X-API-Key')
    if req_api_key != API_KEY:
        return jsonify({"status": "hata", "mesaj": "Yetkisiz istek (Gecersiz API Key)"}), 401
        
    data = request.get_json()
    if not data:
        return jsonify({"status": "hata", "mesaj": "JSON verisi bulunamadi"}), 400
        
    # Eğer tekli nesneyse listeye çevirip ortak işleyelim
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
            # Güç P = V * I (Volt = 220V şebeke gerilimi simülasyonu)
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
    """
    ESP32/Pico durum çekme ucu.
    Röle, LED'ler, Güvenlik Durumu ve OLED web kontrol verilerini döner.
    """
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
    """Dashboard arayüzü için tüm verileri döndürür."""
    return jsonify(system_state), 200

@app.route('/api/control', methods=['POST'])
def control_actuator():
    """Arayüz üzerinden röle, ledler, güvenlik ve OLED'i kontrol etme ucu."""
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
        system_state["oled_mesaj"] = str(value)[:16] # Maksimum 16 karakter
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
    app.run(host='0.0.0.0', port=5000, debug=True)
