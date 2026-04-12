// Augsburg Cache – ESP32 Firmware
//
// Pollt /augsburg/state und zeigt den Fortschritt des Augsburg-Cache
// auf einem NeoPixel-Streifen an. Aufbau und Logik analog zur
// Verstehbahnhof-Firmware (firmware/src/main.cpp), aber für ESP32.
//
// Attract-Farbe: Blau/Cyan (statt Rot/Gelb) → visuell unterscheidbar.

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <time.h>

#include "config.h"

#ifndef TRAIN_FRAME_MS
#define TRAIN_FRAME_MS 18
#endif
#ifndef NUM_WAGONS
#define NUM_WAGONS 3
#endif
#ifndef API_TIMEOUT_MS
#define API_TIMEOUT_MS 2000
#endif
#ifndef ATTRACT_POLL_MS
#define ATTRACT_POLL_MS 10000
#endif
#ifndef FIRE_COOLING
#define FIRE_COOLING 60
#endif
#ifndef FIRE_SPARKING
#define FIRE_SPARKING 110
#endif
#ifndef MAX_BURSTS
#define MAX_BURSTS 6
#endif
#ifndef BURST_FADE
#define BURST_FADE 10
#endif
#ifndef BURST_SPAWN_CHANCE
#define BURST_SPAWN_CHANCE 8
#endif
#ifndef NUM_STATIONS
#define NUM_STATIONS 3
#endif

Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// -------- Farben --------
const uint32_t COLOR_OFF          = Adafruit_NeoPixel::Color(0, 0, 0);
const uint32_t COLOR_TRACK_DONE   = Adafruit_NeoPixel::Color(4, 6, 8);     // dim blaugrau
const uint32_t COLOR_STATION_DONE = Adafruit_NeoPixel::Color(0, 150, 150); // cyan
const uint32_t COLOR_STATION_TODO = Adafruit_NeoPixel::Color(100, 0, 100); // lila
const uint32_t COLOR_TRAIN        = Adafruit_NeoPixel::Color(0, 80, 255);  // blau

uint32_t wagonColor(int idx) {
    const float base_b = 160.0f, base_g = 30.0f;
    float f = 1.0f;
    for (int i = 0; i < idx; i++) f *= 0.55f;
    return Adafruit_NeoPixel::Color(0, (uint8_t)(base_g * f), (uint8_t)(base_b * f));
}

// -------- Zustand --------
int currentStation = -1;
int trainPos = 0;
int trainDir = 1;

// -------- LED-Positionen --------
uint16_t stationLed(int idx) {
    if (idx <= 0) return 0;
    if (idx >= NUM_STATIONS - 1) return NEOPIXEL_COUNT - 1;
    return (uint16_t)(((long)idx * (NEOPIXEL_COUNT - 1)) / (NUM_STATIONS - 1));
}

// -------- Darstellung --------
void drawBaseline() {
    const int cs = (currentStation < 0) ? 0 : currentStation;
    const uint16_t doneUpTo = stationLed(cs);
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) {
        strip.setPixelColor(i, (i <= doneUpTo) ? COLOR_TRACK_DONE : COLOR_OFF);
    }
    for (uint16_t s = 0; s < NUM_STATIONS; s++) {
        uint32_t col = ((int)s <= cs) ? COLOR_STATION_DONE : COLOR_STATION_TODO;
        strip.setPixelColor(stationLed(s), col);
    }
}

void drawTrainWithWagons(int trainLed, int dir, int ledA, int ledB) {
    drawBaseline();
    for (int w = NUM_WAGONS; w >= 1; w--) {
        int wp = trainLed - w * dir;
        if (wp > ledA && wp < ledB) strip.setPixelColor(wp, wagonColor(w));
    }
    if (trainLed >= 0 && trainLed < (int)NEOPIXEL_COUNT) {
        strip.setPixelColor(trainLed, COLOR_TRAIN);
    }
    strip.show();
}

void arrivalAnimation() {
    for (int n = 0; n < 6; n++) {
        for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++)
            strip.setPixelColor(i, COLOR_STATION_DONE);
        strip.show();
        delay(150);
        drawBaseline();
        strip.show();
        delay(150);
    }
}

void openStationAnimation(int newStationIdx) {
    if (newStationIdx < 0 || newStationIdx >= NUM_STATIONS) return;
    const int led = stationLed(newStationIdx);
    for (int i = 0; i < 3; i++) {
        drawBaseline();
        strip.setPixelColor(led, Adafruit_NeoPixel::Color(0, 0, 255));
        strip.show(); delay(180);
        drawBaseline();
        strip.setPixelColor(led, 0);
        strip.show(); delay(120);
    }
    for (int v = 255; v >= 0; v -= 25) {
        drawBaseline();
        strip.setPixelColor(led, Adafruit_NeoPixel::Color(0, 0, v));
        strip.show(); delay(25);
    }
    for (int v = 0; v <= 150; v += 15) {
        drawBaseline();
        strip.setPixelColor(led, Adafruit_NeoPixel::Color(0, v, v));
        strip.show(); delay(25);
    }
    drawBaseline();
    strip.setPixelColor(led, COLOR_STATION_DONE);
    strip.show(); delay(300);
}

void resetTrainToCurrent() {
    trainPos = stationLed(currentStation);
    trainDir = +1;
}

// -------- Attract-Animation (Blau/Cyan statt Rot/Gelb) --------

static uint8_t heat[NEOPIXEL_COUNT];

struct Burst { int16_t led; uint8_t life, r, g, b; };
static Burst bursts[MAX_BURSTS];

static inline uint8_t qadd8(uint8_t a, uint8_t b) {
    uint16_t s = (uint16_t)a + b; return s > 255 ? 255 : (uint8_t)s;
}
static inline uint8_t qsub8(uint8_t a, uint8_t b) { return a > b ? a - b : 0; }

// Blau-Cyan Farbpalette statt Fire2012-Rot
uint32_t coolColor(uint8_t t) {
    uint8_t t192 = (uint16_t)t * 191 / 255;
    uint8_t ramp = (t192 & 0x3F) << 2;
    if (t192 & 0x80)       return Adafruit_NeoPixel::Color(ramp / 4, 255, 255);
    else if (t192 & 0x40)  return Adafruit_NeoPixel::Color(0, ramp, 255);
    else                   return Adafruit_NeoPixel::Color(0, 0, ramp);
}

void fireStep() {
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++)
        heat[i] = qsub8(heat[i], random(0, ((FIRE_COOLING * 10) / NEOPIXEL_COUNT) + 2));
    for (int i = NEOPIXEL_COUNT - 1; i >= 2; i--)
        heat[i] = ((uint16_t)heat[i-1] + heat[i-2] + heat[i-2]) / 3;
    if ((uint8_t)random(0, 256) < FIRE_SPARKING) {
        int y = random(0, 7);
        heat[y] = qadd8(heat[y], random(160, 256));
    }
}

void spawnBurst() {
    int lo = NEOPIXEL_COUNT / 5, hi = NEOPIXEL_COUNT - lo - 1;
    if (hi < lo) { lo = 0; hi = NEOPIXEL_COUNT - 1; }
    for (int i = 0; i < MAX_BURSTS; i++) {
        if (bursts[i].led < 0) {
            bursts[i].led  = random(lo, hi + 1);
            bursts[i].life = 255;
            uint8_t c = random(0, 4);
            switch (c) {
                case 0: bursts[i].r=0;   bursts[i].g=255; bursts[i].b=255; break;
                case 1: bursts[i].r=0;   bursts[i].g=180; bursts[i].b=255; break;
                case 2: bursts[i].r=60;  bursts[i].g=60;  bursts[i].b=255; break;
                case 3: bursts[i].r=255; bursts[i].g=255; bursts[i].b=255; break;
            }
            return;
        }
    }
}

void burstStep() {
    for (int i = 0; i < MAX_BURSTS; i++) {
        if (bursts[i].led < 0) continue;
        if (bursts[i].life <= BURST_FADE) { bursts[i].led = -1; bursts[i].life = 0; }
        else bursts[i].life -= BURST_FADE;
    }
    if ((uint16_t)random(0, 1000) < BURST_SPAWN_CHANCE) spawnBurst();
}

void drawAttract() {
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++)
        strip.setPixelColor(i, coolColor(heat[i]));
    for (int i = 0; i < MAX_BURSTS; i++) {
        if (bursts[i].led < 0) continue;
        uint16_t led = bursts[i].led;
        if (led >= NEOPIXEL_COUNT) continue;
        uint8_t bR = ((uint16_t)bursts[i].r * bursts[i].life) / 255;
        uint8_t bG = ((uint16_t)bursts[i].g * bursts[i].life) / 255;
        uint8_t bB = ((uint16_t)bursts[i].b * bursts[i].life) / 255;
        uint32_t ex = strip.getPixelColor(led);
        uint8_t eR=(ex>>16)&0xFF, eG=(ex>>8)&0xFF, eB=ex&0xFF;
        strip.setPixelColor(led,
            bR>eR?bR:eR, bG>eG?bG:eG, bB>eB?bB:eB);
    }
    strip.show();
}

void tickAttractAnimation() { fireStep(); burstStep(); drawAttract(); }

void transitionFlash() {
    const uint8_t steps[6] = {255, 220, 180, 130, 80, 30};
    for (int s = 0; s < 6; s++) {
        uint8_t v = steps[s];
        for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) {
            uint8_t b = v;
            uint8_t g = (uint16_t)v * (s < 2 ? 255 : (s < 4 ? 180 : 40)) / 255;
            strip.setPixelColor(i, 0, g, b);
        }
        strip.show(); delay(50);
    }
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) heat[i] = 0;
}

// -------- Netzwerk --------

static unsigned long s_wifiLostAt = 0;

void connectWifi() {
    Serial.printf("Verbinde mit WLAN %s...\n", WIFI_SSID);
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);           // Modem-Sleep aus (Stabilität)
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(250); Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WLAN OK, IP: %s\n", WiFi.localIP().toString().c_str());
        configTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com");
        Serial.println("NTP gestartet");
    } else {
        Serial.println("WLAN fehlgeschlagen, reboot in 5s");
        delay(5000);
        ESP.restart();
    }
}

void maintainWifi() {
    static unsigned long s_lastCheck = 0;
    unsigned long now = millis();
    if (now - s_lastCheck < 15000UL) return;
    s_lastCheck = now;

    if (WiFi.status() == WL_CONNECTED) {
        if (s_wifiLostAt != 0) {
            Serial.printf("WiFi wieder verbunden, IP: %s\n",
                          WiFi.localIP().toString().c_str());
            s_wifiLostAt = 0;
        }
        return;
    }
    if (s_wifiLostAt == 0) {
        s_wifiLostAt = now;
        Serial.println("WiFi getrennt – Reconnect…");
        WiFi.reconnect();
    } else if (now - s_wifiLostAt > 5UL * 60UL * 1000UL) {
        Serial.println("WiFi seit 5 Min weg – Neustart!");
        delay(200); ESP.restart();
    }
}

void checkNightReset() {
    static unsigned long s_lastCheck = 0;
    unsigned long now_ms = millis();
    if (now_ms - s_lastCheck < 60000UL) return;
    s_lastCheck = now_ms;

    if (now_ms > 26UL * 3600UL * 1000UL) {
        Serial.println("26h-Fallback-Neustart");
        delay(200); ESP.restart();
    }
    time_t now_t = time(nullptr);
    struct tm* t = localtime(&now_t);
    if (t->tm_year > 120 && t->tm_hour == 3 && t->tm_min == 0) {
        Serial.println("Nacht-Neustart 3:00 Uhr MEZ");
        delay(200); ESP.restart();
    }
}

// Forward-Declaration
void pollApiAndUpdate();

int fetchCurrentStation() {
    if (WiFi.status() != WL_CONNECTED) return -1;
    HTTPClient http;
    if (!http.begin(API_URL)) {
        Serial.println("API: http.begin() fehlgeschlagen");
        return -1;
    }
    http.setTimeout(API_TIMEOUT_MS);
    int code = http.GET();
    if (code != 200) {
        Serial.printf("API HTTP %d\n", code);
        http.end(); return -1;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) { Serial.printf("JSON Fehler: %s\n", err.c_str()); return -1; }
    return doc["station"] | -1;
}

void pollApiAndUpdate() {
    Serial.println("API: check…");
    int s = fetchCurrentStation();
    if (s < -1 || s >= NUM_STATIONS) return;
    if (s == currentStation) return;

    const int prev = currentStation;
    if (prev >= 0 && s > prev) openStationAnimation(s);

    currentStation = s;
    if (s >= 0) {
        Serial.printf("Station: %d\n", s);
        resetTrainToCurrent();
        if (s == NUM_STATIONS - 1 && prev != s) {
            Serial.println("Finale!");
            arrivalAnimation();
        }
    } else {
        Serial.println("Idle (Server-Timeout)");
    }
}

// -------- Pendel --------
void tickTrainAnimation() {
    if (currentStation >= NUM_STATIONS - 1) {
        drawBaseline(); strip.show(); return;
    }
    const int cs   = (currentStation < 0) ? 0 : currentStation;
    const int ledA = stationLed(cs);
    const int ledB = stationLed(cs + 1);
    const int minPos = ledA + 1, maxPos = ledB - 1;
    if (maxPos < minPos) { drawBaseline(); strip.show(); return; }

    trainPos += trainDir;
    bool reachedNext = false;
    if (trainPos >= maxPos) { trainPos = maxPos; trainDir = -1; reachedNext = true; }
    else if (trainPos <= minPos) { trainPos = minPos; trainDir = +1; }

    drawTrainWithWagons(trainPos, trainDir, ledA, ledB);
    if (reachedNext) pollApiAndUpdate();
}

// -------- Setup / Loop --------

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nAugsburg Cache Firmware");

    for (int i = 0; i < MAX_BURSTS; i++) bursts[i] = {-1, 0, 0, 0, 0};
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) heat[i] = 0;

    strip.begin();
    strip.setBrightness(NEOPIXEL_BRIGHTNESS);
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) strip.setPixelColor(i, 0);
    strip.show();

    connectWifi();
    pollApiAndUpdate();
}

void loop() {
    maintainWifi();
    checkNightReset();

    const unsigned long now = millis();

    if (currentStation < 0) {
        static unsigned long lastAttractPoll = 0;
        if (now - lastAttractPoll >= ATTRACT_POLL_MS) {
            lastAttractPoll = now;
            int prev = currentStation;
            pollApiAndUpdate();
            if (prev < 0 && currentStation >= 0) transitionFlash();
        }
        static unsigned long lastAttractFrame = 0;
        if (now - lastAttractFrame >= TRAIN_FRAME_MS) {
            lastAttractFrame = now;
            tickAttractAnimation();
        }
    } else if (currentStation >= NUM_STATIONS - 1) {
        static unsigned long lastFinalePoll = 0;
        if (now - lastFinalePoll >= ATTRACT_POLL_MS) {
            lastFinalePoll = now;
            pollApiAndUpdate();
        }
        static unsigned long lastFinaleFrame = 0;
        if (now - lastFinaleFrame >= TRAIN_FRAME_MS) {
            lastFinaleFrame = now;
            tickTrainAnimation();
        }
    } else {
        static unsigned long lastFrame = 0;
        if (now - lastFrame >= TRAIN_FRAME_MS) {
            lastFrame = now;
            tickTrainAnimation();
        }
    }
}
