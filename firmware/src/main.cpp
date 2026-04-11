// Verstehbahnhof-Cache – Wemos D1 Mini Firmware
//
// Pollt eine API (wird parallel von MatzE gebaut) nach dem aktuellen
// Bahnhof und animiert Emma auf dem NeoPixel-Streifen von ihrem
// bisherigen Halt zum neuen Halt.
//
// API-Antwort (Beispiel):
//   { "currentStation": 2, "totalStations": 5 }

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>

#include "config.h"

Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

static const uint16_t NUM_STATIONS =
    sizeof(STATION_LEDS) / sizeof(STATION_LEDS[0]);

// aktueller Bahnhof laut letztem API-Abruf (−1 = noch nichts animiert)
int currentStation = -1;

// -------- Farben --------
const uint32_t COLOR_TRACK    = Adafruit_NeoPixel::Color(8, 8, 20);    // dunkelblau
const uint32_t COLOR_STATION  = Adafruit_NeoPixel::Color(40, 40, 40);  // weiss
const uint32_t COLOR_TRAIN    = Adafruit_NeoPixel::Color(255, 40, 0);  // Emma orange-rot
const uint32_t COLOR_GOAL     = Adafruit_NeoPixel::Color(0, 180, 60);  // Puppenkiste grün

// -------- Darstellung --------

void drawBaseline() {
    // Schiene
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) {
        strip.setPixelColor(i, COLOR_TRACK);
    }
    // Bahnhöfe
    for (uint16_t s = 0; s < NUM_STATIONS; s++) {
        strip.setPixelColor(STATION_LEDS[s], COLOR_STATION);
    }
    // Ziel (Augsburg) hervorheben
    strip.setPixelColor(STATION_LEDS[NUM_STATIONS - 1], COLOR_GOAL);
}

void drawTrainAt(uint16_t led) {
    drawBaseline();
    strip.setPixelColor(led, COLOR_TRAIN);
    strip.show();
}

// Fährt Emma LED für LED vom Bahnhof 'from' zum Bahnhof 'to'.
// Unterstützt Vorwärts und Rückwärts.
void animateTrain(uint16_t from, uint16_t to) {
    const uint16_t ledFrom = STATION_LEDS[from];
    const uint16_t ledTo   = STATION_LEDS[to];
    const int step = (ledTo >= ledFrom) ? 1 : -1;

    for (int led = ledFrom; led != ledTo; led += step) {
        drawTrainAt(led);
        delay(120);  // Geschwindigkeit der Fahrt
    }
    drawTrainAt(ledTo);

    // kleiner "Halt"-Blinker am Bahnhof
    for (int i = 0; i < 2; i++) {
        delay(150);
        strip.setPixelColor(ledTo, COLOR_STATION);
        strip.show();
        delay(150);
        strip.setPixelColor(ledTo, COLOR_TRAIN);
        strip.show();
    }
}

void arrivalAnimation() {
    // kurze Party-Animation, wenn Emma in Augsburg ankommt
    for (int n = 0; n < 5; n++) {
        for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) {
            strip.setPixelColor(i, COLOR_GOAL);
        }
        strip.show();
        delay(150);
        drawBaseline();
        strip.show();
        delay(150);
    }
}

// -------- Netzwerk --------

void connectWifi() {
    Serial.printf("Verbinde mit WLAN %s...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WLAN OK, IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("WLAN fehlgeschlagen, reboot in 5s");
        delay(5000);
        ESP.restart();
    }
}

// Holt currentStation von der API. Gibt -1 bei Fehler zurück.
int fetchCurrentStation() {
    if (WiFi.status() != WL_CONNECTED) return -1;

    WiFiClient client;
    HTTPClient http;
    http.begin(client, API_URL);
    http.setTimeout(3000);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("API HTTP %d\n", code);
        http.end();
        return -1;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) {
        Serial.printf("JSON Fehler: %s\n", err.c_str());
        return -1;
    }

    int cs = doc["currentStation"] | -1;
    return cs;
}

// -------- Setup / Loop --------

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nVerstehbahnhof-Cache Firmware");

    strip.begin();
    strip.setBrightness(NEOPIXEL_BRIGHTNESS);
    drawBaseline();
    // Emma beim Start auf Bahnhof 0 setzen
    strip.setPixelColor(STATION_LEDS[0], COLOR_TRAIN);
    strip.show();

    connectWifi();
}

void loop() {
    static unsigned long lastPoll = 0;
    if (millis() - lastPoll < API_POLL_MS) {
        delay(20);
        return;
    }
    lastPoll = millis();

    int newStation = fetchCurrentStation();
    if (newStation < 0 || newStation >= (int)NUM_STATIONS) return;

    if (currentStation < 0) {
        // erster Abruf: Emma ohne Animation an die richtige Stelle setzen
        currentStation = newStation;
        drawTrainAt(STATION_LEDS[currentStation]);
        Serial.printf("Start auf Bahnhof %d\n", currentStation);
        return;
    }

    if (newStation != currentStation) {
        Serial.printf("Fahrt: Bahnhof %d -> %d\n", currentStation, newStation);
        animateTrain(currentStation, newStation);
        currentStation = newStation;

        if (currentStation == (int)NUM_STATIONS - 1) {
            Serial.println("Augsburg erreicht!");
            arrivalAnimation();
            drawTrainAt(STATION_LEDS[currentStation]);
        }
    }
}
