#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include "wifi_prov.h"

#define UDP_PORT       8266

#define REED_PIN       D1
#define CURRENT_PIN    A0

double adc_zero_offset = 800.0;
const double sensitivity = 0.185;

WiFiUDP Udp;

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n==================================");
    Serial.println("ESP-12E Düğüm 2: Enerji ve Manyetik");
    Serial.println("==================================");

    pinMode(REED_PIN, INPUT);

    Serial.println("Akım sensörü sıfır noktası kalibre ediliyor...");
    double sum = 0;
    for (int i = 0; i < 100; i++) {
        sum += analogRead(CURRENT_PIN);
        delay(10);
    }
    adc_zero_offset = sum / 100.0;
    Serial.print("Kalibrasyon Tamamlandı! Sıfır Akım ADC Değeri: ");
    Serial.println(adc_zero_offset);

    wifiProvBegin("Kurulum-Dugum2");

    Serial.print("Alinan IP Adresi: ");
    Serial.println(WiFi.localIP());

    Udp.begin(UDP_PORT);
    Serial.printf("UDP yayini port %d uzerinden hazir.\n", UDP_PORT);
}

void loop() {

    int magnetic_state = digitalRead(REED_PIN);

    double voltage_sum = 0;
    int samples = 0;
    unsigned long start_time = millis();

    while (millis() - start_time < 40) {
        double raw_adc = analogRead(CURRENT_PIN);
        double voltage = (raw_adc / 1023.0) * 3.2;
        double zero_voltage = (adc_zero_offset / 1023.0) * 3.2;
        double diff = voltage - zero_voltage;
        voltage_sum += diff * diff;
        samples++;
        delayMicroseconds(500);
    }

    double rms_voltage = sqrt(voltage_sum / samples);
    double current_amps = rms_voltage / sensitivity;
    double current_mA = current_amps * 1000.0;

    static double smoothed_current_mA = -1.0;
    if (smoothed_current_mA < 0.0) {
        smoothed_current_mA = current_mA;
    } else {
        smoothed_current_mA = (smoothed_current_mA * 0.85) + (current_mA * 0.15);
    }

    current_mA = smoothed_current_mA;

    Serial.println("\n========================================================");
    Serial.printf("Manyetik Durum : %s\n", (magnetic_state == 1) ? "KAPI KAPALI (Mıknatıs Algılandı)" : "KAPI AÇIK (Mıknatıs Yok)");
    Serial.printf("Akım (RMS)     : %.2f mA\n", current_mA);
    Serial.println("========================================================");

    if (WiFi.status() == WL_CONNECTED) {
        String json = "{";
        json += "\"node_id\":2,";
        json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
        json += "\"current_mA\":" + String(current_mA, 2) + ",";
        json += "\"door\":" + String(magnetic_state == 1 ? 1 : 0);
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

    bool acil_durum = (magnetic_state == 0);
    delay(acil_durum ? 150 : 500);
}
