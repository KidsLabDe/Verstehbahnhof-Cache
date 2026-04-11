// Verstehbahnhof-Cache – Wemos D1 Mini Lite Firmware
//
// Pollt eine API (wird parallel von MatzE gebaut) nach dem aktuellen
// Bahnhof und zeigt Emmas Reise von Fürstenberg nach Augsburg auf
// dem NeoPixel-Streifen.
//
// Darstellung:
//  - Bahnhöfe gleichmäßig über den Streifen verteilt
//  - bereits erreichte Bahnhöfe: grün
//  - noch nicht erreichte Bahnhöfe: rot
//  - bereits befahrene Strecke: dim weiß
//  - noch nicht befahrene Strecke: aus
//  - der Zug (blau) pendelt zwischen aktuellem und nächstem Bahnhof
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

#ifndef TRAIN_FRAME_MS
#define TRAIN_FRAME_MS 18
#endif
#ifndef NUM_WAGONS
#define NUM_WAGONS 3
#endif
#ifndef API_TIMEOUT_MS
#define API_TIMEOUT_MS 1500
#endif

Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// -------- Bahnhöfe --------
// Namen werden nur fürs Serial-Log benutzt. Positionen auf dem Streifen
// werden automatisch gleichmäßig verteilt (siehe stationLed()).
const char* STATION_NAMES[] = {
    "Lok (Fürstenberg)",
    "Repair Cafe (Berlin)",
    "Goldstein (Leipzig)",
    "Loren (Nürnberg)",
    "Augsburg (Puppenkiste)"
};
static const uint16_t NUM_STATIONS =
    sizeof(STATION_NAMES) / sizeof(STATION_NAMES[0]);

// LED-Position eines Bahnhofs, gleichmäßig über den Streifen verteilt.
// Station 0 = LED 0, letzte Station = letzte LED.
uint16_t stationLed(int idx) {
    if (idx <= 0) return 0;
    if (idx >= (int)NUM_STATIONS - 1) return NEOPIXEL_COUNT - 1;
    return (uint16_t)(((long)idx * (NEOPIXEL_COUNT - 1)) / (NUM_STATIONS - 1));
}

// -------- Farben --------
const uint32_t COLOR_OFF           = Adafruit_NeoPixel::Color(0, 0, 0);
const uint32_t COLOR_TRACK_DONE    = Adafruit_NeoPixel::Color(6, 6, 6);     // dim weiß
const uint32_t COLOR_STATION_DONE  = Adafruit_NeoPixel::Color(0, 150, 0);   // grün
const uint32_t COLOR_STATION_TODO  = Adafruit_NeoPixel::Color(150, 0, 0);   // rot
const uint32_t COLOR_TRAIN         = Adafruit_NeoPixel::Color(0, 40, 220);  // blau (Lok)

// Waggons werden mit abnehmender Helligkeit hinter der Lok gezeichnet.
// Die Basisfarbe ist ein dunkleres Blau als die Lok, pro Waggon wird weiter
// gedimmt. Wird programmatisch in wagonColor() berechnet.
uint32_t wagonColor(int idx /* 1..NUM_WAGONS */) {
    // Faktor 0.55, 0.30, 0.15, ...
    const float base_r = 0.0f, base_g = 20.0f, base_b = 140.0f;
    float f = 1.0f;
    for (int i = 0; i < idx; i++) f *= 0.55f;
    return Adafruit_NeoPixel::Color(
        (uint8_t)(base_r * f),
        (uint8_t)(base_g * f),
        (uint8_t)(base_b * f));
}

// -------- Zustand --------
int currentStation = -1;  // letzter bekannter Bahnhof (−1 = noch nix)
int trainPos = 0;          // aktuelle LED-Position des Zugs
int trainDir = 1;          // Pendel-Richtung: +1 vorwärts, -1 rückwärts

// -------- Darstellung --------

// Malt Strecke + Bahnhöfe (ohne Zug) auf den Puffer.
// Basiert auf currentStation (kann -1 sein).
void drawBaseline() {
    const int cs = (currentStation < 0) ? 0 : currentStation;
    const uint16_t doneUpTo = stationLed(cs);  // bis hierhin gilt Strecke als befahren

    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) {
        strip.setPixelColor(i, (i <= doneUpTo) ? COLOR_TRACK_DONE : COLOR_OFF);
    }

    for (uint16_t s = 0; s < NUM_STATIONS; s++) {
        uint32_t col = ((int)s <= cs) ? COLOR_STATION_DONE : COLOR_STATION_TODO;
        strip.setPixelColor(stationLed(s), col);
    }
}

// Malt Lok + Waggons. Waggons liegen entgegen der Fahrtrichtung hinter
// der Lok und werden nicht auf Bahnhofs-LEDs gezeichnet (sie "verschwinden"
// dort im Bahnhof). ledA/ledB sind die LED-Positionen der beiden Bahnhöfe,
// zwischen denen der Zug gerade pendelt.
void drawTrainWithWagons(int trainLed, int dir, int ledA, int ledB) {
    drawBaseline();

    // Waggons zuerst, Lok darüber (falls Überlappung)
    for (int w = NUM_WAGONS; w >= 1; w--) {
        int wp = trainLed - w * dir;
        if (wp > ledA && wp < ledB) {
            strip.setPixelColor(wp, wagonColor(w));
        }
    }

    if (trainLed >= 0 && trainLed < (int)NEOPIXEL_COUNT) {
        strip.setPixelColor(trainLed, COLOR_TRAIN);
    }
    strip.show();
}

void arrivalAnimation() {
    // kurze Party, wenn Emma in Augsburg ankommt
    for (int n = 0; n < 6; n++) {
        for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) {
            strip.setPixelColor(i, COLOR_STATION_DONE);
        }
        strip.show();
        delay(150);
        drawBaseline();
        strip.show();
        delay(150);
    }
}

// Setzt die Zug-Position passend zum aktuellen Bahnhof zurück.
void resetTrainToCurrent() {
    trainPos = stationLed(currentStation);
    trainDir = +1;
}

// Bewegt den Zug einen Schritt weiter und zeichnet das Bild.
// Der Zug pendelt zwischen currentStation und currentStation+1.
// Ohne API-Status (currentStation < 0) pendelt er zwischen Bahnhof 0 und 1.
// Am Ziel (letzter Bahnhof) steht der Zug still.
void tickTrainAnimation() {
    // Am Ziel angekommen: keine Bewegung, alles grün, kein Zug gezeichnet
    if (currentStation >= (int)NUM_STATIONS - 1) {
        drawBaseline();
        strip.show();
        return;
    }

    const int cs = (currentStation < 0) ? 0 : currentStation;
    const int ledA = stationLed(cs);
    const int ledB = stationLed(cs + 1);

    // Zug soll die Bahnhofs-LEDs nicht überfahren: eine LED davor umkehren.
    const int minPos = ledA + 1;
    const int maxPos = ledB - 1;

    // Wenn die Bahnhöfe direkt nebeneinander liegen, gibt es keinen Platz
    // für den Zug dazwischen – dann lieber gar nichts malen.
    if (maxPos < minPos) {
        drawBaseline();
        strip.show();
        return;
    }

    trainPos += trainDir;
    if (trainPos >= maxPos) {
        trainPos = maxPos;
        trainDir = -1;
    } else if (trainPos <= minPos) {
        trainPos = minPos;
        trainDir = +1;
    }

    drawTrainWithWagons(trainPos, trainDir, ledA, ledB);
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
    http.setTimeout(API_TIMEOUT_MS);

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

    return doc["currentStation"] | -1;
}

// -------- Setup / Loop --------

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nVerstehbahnhof-Cache Firmware");

    strip.begin();
    strip.setBrightness(NEOPIXEL_BRIGHTNESS);
    drawBaseline();
    strip.show();

    connectWifi();
}

void loop() {
    const unsigned long now = millis();

    // --- API-Polling ---
    static unsigned long lastPoll = 0;
    if (now - lastPoll >= API_POLL_MS) {
        lastPoll = now;
        int s = fetchCurrentStation();
        if (s >= 0 && s < (int)NUM_STATIONS && s != currentStation) {
            const int prev = currentStation;
            currentStation = s;
            Serial.printf("Bahnhof: %d (%s)\n",
                          currentStation, STATION_NAMES[currentStation]);

            // Zug auf den neuen Bahnhof setzen und von dort aus pendeln
            resetTrainToCurrent();

            // Ziel erreicht? → Ankunfts-Show
            if (currentStation == (int)NUM_STATIONS - 1 && prev != currentStation) {
                Serial.println("Augsburg erreicht!");
                arrivalAnimation();
            }
        }
    }

    // --- Zug-Animation (pendelt zwischen currentStation und currentStation+1) ---
    static unsigned long lastFrame = 0;
    if (now - lastFrame >= TRAIN_FRAME_MS) {
        lastFrame = now;
        tickTrainAnimation();
    }
}
