#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <DHT.h>
#include "wifi_prov.h"

#define UDP_PORT       8266

#define DHTPIN         D2
#define DHTTYPE        DHT22
#define FLAME_PIN      D1
#define GAS_PIN        A0

DHT dht(DHTPIN, DHTTYPE);
WiFiUDP Udp;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n==================================");
    Serial.println("ESP-12E Düğüm 1: Gaz, Alev ve Ortam");
    Serial.println("==================================");

    pinMode(FLAME_PIN, INPUT);

    dht.begin();

    wifiProvBegin("Kurulum-Dugum1");

    Serial.print("Alinan IP Adresi: ");
    Serial.println(WiFi.localIP());

    Udp.begin(UDP_PORT);
    Serial.printf("UDP yayini port %d uzerinden hazir.\n", UDP_PORT);
}

void loop() {

    static float temperature = 0.0;
    static float humidity = 0.0;
    static unsigned long last_dht_read = 0;
    unsigned long now_ms = millis();
    if (last_dht_read == 0 || (now_ms - last_dht_read) >= 2000) {
        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (isnan(t) || isnan(h)) {
            Serial.println("[HATA] DHT22 verisi okunamıyor!");
        } else {
            temperature = t;
            humidity = h;
        }
        last_dht_read = now_ms;
    }

    int gas_analog = analogRead(GAS_PIN);
    int flame_digital = digitalRead(FLAME_PIN);

    Serial.println("\n========================================================");
    Serial.printf("Sıcaklık       : %.2f °C\n", temperature);
    Serial.printf("Nem            : %.2f %%\n", humidity);
    Serial.printf("Gaz Analog (A0): %d\n", gas_analog);
    Serial.printf("Alev Durumu    : %s\n", (flame_digital == 0) ? "ALEV ALGILANDI! (Ateş)" : "Güvenli (Alev yok)");
    Serial.println("========================================================");

    if (WiFi.status() == WL_CONNECTED) {

        String json = "{";
        json += "\"node_id\":1,";
        json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
        json += "\"temperature\":" + String(temperature, 2) + ",";
        json += "\"humidity\":" + String(humidity, 2) + ",";
        json += "\"gas\":" + String(gas_analog) + ",";
        json += "\"flame\":" + String(flame_digital == 0 ? 1 : 0);
        json += "}";

        Serial.print("[UDP] Gonderilen JSON: ");
        Serial.println(json);

        Udp.beginPacket(IPAddress(255, 255, 255, 255), UDP_PORT);
        Udp.write(json.c_str());
        int result = Udp.endPacket();

        if (result == 1) {
            Serial.println("[UDP] Paket başarıyla yayınlandı.");
        } else {
            Serial.println("[UDP] Paket yayını başarısız!");
        }
    } else {
        Serial.println("[WIFI] Baglanti yok! UDP yayını yapılamadı.");
    }

    bool acil_durum = (flame_digital == 0) || (gas_analog >= 400);
    delay(acil_durum ? 150 : 500);
}
