from flask import Flask, render_template, jsonify, request
from flask_cors import CORS
import time

app = Flask(__name__)
CORS(app)

# Global sistem durumu (Başlangıç verileri)
system_state = {
    # İstasyon 1: Aydınlatma & Güç Tüketimi
    "istasyon_1": {
        "akim_mA": 0.0,
        "guc_W": 0.0,
        "rele": "kapali"
    },
    # İstasyon 2: Daire Kapısı (Güvenlik)
    "istasyon_2": {
        "kapi": "kapali",
        "mesafe_cm": 150.0,
        "guvenlik_aktif": True
    },
    # İstasyon 3: Gaz & Alev (Mutfak)
    "istasyon_3": {
        "gaz": 120,          # MQ-9 ADC/PPM değeri
        "alev": 0,           # 0: alev yok, 1: alev var
        "gaz_esik": 400
    },
    # İstasyon 4: Deprem & Çevre (Salon Kolonu)
    "istasyon_4": {
        "ivme_g": 1.0,       # MPU-6050 bileşke ivmesi (1.0g durağan)
        "sicaklik_C": 24.5,
        "nem_Yuzde": 45.0,
        "deprem_esik": 1.5
    },
    # ESP32 GND Test Durumu (Aktif test edilen GPIO'lar)
    "esp32_gnd_test": {
        "gnd_pins": [],
        "son_guncelleme": ""
    },
    # Genel Aktüatör Durumları (LED'ler)
    "led_status": ["kapali", "kapali", "kapali"], # LED1, LED2, LED3
    "merkezi_alarm": "pasif"
}

# Güvenlik için API Anahtarı (Rapor ile uyumlu)
API_KEY = "tasarim_projesi_secret_key"

def alarm_denetimi():
    """Sensör eşik değerlerine göre alarm durumunu günceller."""
    alarm = False
    
    # Mutfak Gaz Kaçağı
    if system_state["istasyon_3"]["gaz"] >= system_state["istasyon_3"]["gaz_esik"]:
        alarm = True
        
    # Mutfak Yangın/Alev
    if system_state["istasyon_3"]["alev"] == 1:
        alarm = True
        
    # Deprem / Şiddetli Sarsıntı
    if system_state["istasyon_4"]["ivme_g"] >= system_state["istasyon_4"]["deprem_esik"]:
        alarm = True
        
    # İzinsiz Giriş (Güvenlik aktifken kapı açılırsa)
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
    İstasyon bazlı verileri alır ve günceller.
    """
    # X-API-Key doğrulaması
    req_api_key = request.headers.get('X-API-Key')
    if req_api_key != API_KEY:
        return jsonify({"status": "hata", "mesaj": "Yetkisiz istek (Gecersiz API Key)"}), 401
        
    data = request.get_json()
    if not data:
        return jsonify({"status": "hata", "mesaj": "JSON verisi bulunamadi"}), 400
        
    istasyon_id = data.get("istasyon_id")
    
    if istasyon_id == 1:
        system_state["istasyon_1"]["akim_mA"] = float(data.get("akim_mA", 0.0))
        # Güç P = V * I (Volt = 220V şebeke gerilimi simülasyonu)
        system_state["istasyon_1"]["guc_W"] = round((220.0 * system_state["istasyon_1"]["akim_mA"]) / 1000.0, 2)
        
    elif istasyon_id == 2:
        system_state["istasyon_2"]["kapi"] = data.get("kapi", "kapali")
        system_state["istasyon_2"]["mesafe_cm"] = float(data.get("mesafe_cm", 150.0))
        
    elif istasyon_id == 3:
        system_state["istasyon_3"]["gaz"] = int(data.get("gaz", 120))
        system_state["istasyon_3"]["alev"] = int(data.get("alev", 0))
        
    elif istasyon_id == 4:
        system_state["istasyon_4"]["ivme_g"] = float(data.get("ivme_g", 1.0))
        system_state["istasyon_4"]["sicaklik_C"] = float(data.get("sicaklik_C", 24.5))
        system_state["istasyon_4"]["nem_Yuzde"] = float(data.get("nem_Yuzde", 45.0))
        
    # GND Test Uç Noktası (Kullanıcı ESP32 ile pin testi yapıyorsa)
    elif istasyon_id == 99:
        system_state["esp32_gnd_test"]["gnd_pins"] = data.get("gnd_pins", [])
        system_state["esp32_gnd_test"]["son_guncelleme"] = time.strftime("%H:%M:%S")

    alarm_denetimi()
    return jsonify({"status": "basarili", "merkezi_alarm": system_state["merkezi_alarm"]}), 200

@app.route('/status', methods=['GET'])
def get_status():
    """
    Raporla uyumlu aktüatör durumu alma ucu.
    Gömülü cihazlar (ESP32/Pico) bu uçtan röle ve LED durumlarını çeker.
    """
    # X-API-Key doğrulaması
    req_api_key = request.headers.get('X-API-Key')
    if req_api_key != API_KEY:
         return jsonify({"status": "hata", "mesaj": "Yetkisiz istek"}), 401
         
    return jsonify({
        "rele": system_state["istasyon_1"]["rele"],
        "ledler": [x.capitalize() for x in system_state["led_status"]]  # Raporla uyumlu: ["Acik", "Kapali", ...]
    }), 200

@app.route('/api/data', methods=['GET'])
def get_all_data():
    """Dashboard arayüzü için tüm verileri döndürür."""
    return jsonify(system_state), 200

@app.route('/api/control', methods=['POST'])
def control_actuator():
    """Arayüz üzerinden röle, ledler ve alarmı kontrol etme ucu."""
    data = request.get_json()
    if not data:
        return jsonify({"status": "hata", "mesaj": "Gecersiz istek"}), 400
        
    target = data.get("target")
    value = data.get("value")
    
    if target == "rele":
        system_state["istasyon_1"]["rele"] = "acik" if value == "acik" else "kapali"
    elif target == "led":
        idx = int(data.get("index", 0))
        if 0 <= idx < 3:
            system_state["led_status"][idx] = "acik" if value == "acik" else "kapali"
    elif target == "guvenlik":
        system_state["istasyon_2"]["guvenlik_aktif"] = bool(value)
    elif target == "alarm_sifirla":
        # Alarmı ve sensörleri geçici olarak güvenli sınıra çeker
        system_state["istasyon_3"]["gaz"] = 120
        system_state["istasyon_3"]["alev"] = 0
        system_state["istasyon_4"]["ivme_g"] = 1.0
        system_state["istasyon_2"]["kapi"] = "kapali"
        system_state["merkezi_alarm"] = "pasif"
        
    alarm_denetimi()
    return jsonify({"status": "basarili", "state": system_state}), 200

if __name__ == '__main__':
    print("--------------------------------------------------")
    print("Akilli Ev Projesi Yerel Sunucu Baslatiliyor...")
    print("Adres: http://127.0.0.1:5000")
    print("ESP32 baglantisi icin bilgisayarinizin IP adresini kullanin.")
    print("--------------------------------------------------")
    app.run(host='0.0.0.0', port=5000, debug=True)
