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
#include <time.h>

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
#ifndef ATTRACT_POLL_MS
#define ATTRACT_POLL_MS 10000
#endif
#ifndef FIRE_COOLING
#define FIRE_COOLING 55
#endif
#ifndef FIRE_SPARKING
#define FIRE_SPARKING 120
#endif
#ifndef MAX_BURSTS
#define MAX_BURSTS 6
#endif
#ifndef BURST_FADE
#define BURST_FADE 12
#endif
#ifndef BURST_SPAWN_CHANCE
#define BURST_SPAWN_CHANCE 8
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

// Forward-Declaration (tickTrainAnimation ruft pollApiAndUpdate auf)
void pollApiAndUpdate();

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

// Spielt die "Bahnhof wird geöffnet"-Animation auf der LED-Position des
// übergebenen Bahnhofs. Phasen: 3x rot blinken → langsam rot ausfaden →
// langsam grün einfaden. Restlicher Streifen wird in seiner Baseline
// gehalten. Blockierend, ~1.8 s. Wird aufgerufen, BEVOR currentStation
// auf den neuen Wert gesetzt wird, damit drawBaseline() den Bahnhof
// noch als rot kennt und die Farben sauber übereinander laufen.
void openStationAnimation(int newStationIdx) {
    if (newStationIdx < 0 || newStationIdx >= (int)NUM_STATIONS) return;
    const int led = stationLed(newStationIdx);

    // Phase 1: kräftiges rotes Blinken
    for (int i = 0; i < 3; i++) {
        drawBaseline();
        strip.setPixelColor(led, Adafruit_NeoPixel::Color(255, 0, 0));
        strip.show();
        delay(180);
        drawBaseline();
        strip.setPixelColor(led, 0);
        strip.show();
        delay(120);
    }
    // Phase 2: rot → schwarz faden
    for (int v = 255; v >= 0; v -= 25) {
        drawBaseline();
        strip.setPixelColor(led, Adafruit_NeoPixel::Color(v, 0, 0));
        strip.show();
        delay(25);
    }
    // Phase 3: schwarz → grün faden
    for (int v = 0; v <= 150; v += 15) {
        drawBaseline();
        strip.setPixelColor(led, Adafruit_NeoPixel::Color(0, v, 0));
        strip.show();
        delay(25);
    }
    // kurz grün halten, damit der "Klick" sichtbar ist
    drawBaseline();
    strip.setPixelColor(led, Adafruit_NeoPixel::Color(0, 150, 0));
    strip.show();
    delay(300);
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
    bool reachedNext = false;
    if (trainPos >= maxPos) {
        trainPos = maxPos;
        trainDir = -1;
        reachedNext = true;  // wir stehen vorm nächsten (roten) Bahnhof
    } else if (trainPos <= minPos) {
        trainPos = minPos;
        trainDir = +1;
    }

    drawTrainWithWagons(trainPos, trainDir, ledA, ledB);

    // Am Wendepunkt vor dem nächsten roten Bahnhof: API fragen, ob es
    // inzwischen weitergeht. Das ist der einzige Zeitpunkt, an dem die
    // Firmware pollt – kein Zeittakt.
    if (reachedNext) {
        pollApiAndUpdate();
    }
}

// -------- Attract-Animation (Feuer + Feuerwerk) --------
//
// Aktiv wenn noch niemand den Initial-QR gescannt hat (currentStation < 0).
// Fire2012 als Basis, darüber gelayerte Bursts als Feuerwerk.

static uint8_t heat[NEOPIXEL_COUNT];

struct Burst {
    int16_t led;   // LED-Position; -1 = Slot frei
    uint8_t life;  // 0..255, fadet ab
    uint8_t r, g, b;
};
static Burst bursts[MAX_BURSTS];

static inline uint8_t qadd8(uint8_t a, uint8_t b) {
    uint16_t s = (uint16_t)a + b;
    return s > 255 ? 255 : (uint8_t)s;
}
static inline uint8_t qsub8(uint8_t a, uint8_t b) {
    return a > b ? a - b : 0;
}

// Fire2012-Palette: schwarz → rot → gelb → weiß
uint32_t heatColor(uint8_t t) {
    uint8_t t192 = (uint16_t)t * 191 / 255;  // 0..191
    uint8_t heatramp = t192 & 0x3F;          // 0..63
    heatramp <<= 2;                          // 0..252

    if (t192 & 0x80) {
        // obersten Drittel: gelb → weiß (r=255, g=255, b=ramp)
        return Adafruit_NeoPixel::Color(255, 255, heatramp);
    } else if (t192 & 0x40) {
        // mittleres Drittel: rot → gelb (r=255, g=ramp, b=0)
        return Adafruit_NeoPixel::Color(255, heatramp, 0);
    } else {
        // unteres Drittel: schwarz → rot (r=ramp, g=0, b=0)
        return Adafruit_NeoPixel::Color(heatramp, 0, 0);
    }
}

void fireStep() {
    // 1. Abkühlen
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) {
        uint8_t cooldown = random(0, ((FIRE_COOLING * 10) / NEOPIXEL_COUNT) + 2);
        heat[i] = qsub8(heat[i], cooldown);
    }
    // 2. Hitze nach oben driften
    for (int i = NEOPIXEL_COUNT - 1; i >= 2; i--) {
        heat[i] = ((uint16_t)heat[i - 1] + heat[i - 2] + heat[i - 2]) / 3;
    }
    // 3. Zufällig neu zünden (unten)
    if ((uint8_t)random(0, 256) < FIRE_SPARKING) {
        int y = random(0, 7);
        heat[y] = qadd8(heat[y], random(160, 256));
    }
}

void spawnBurst() {
    // mittlere 60 % des Streifens
    int lo = NEOPIXEL_COUNT / 5;
    int hi = NEOPIXEL_COUNT - lo - 1;
    if (hi < lo) { lo = 0; hi = NEOPIXEL_COUNT - 1; }

    for (int i = 0; i < MAX_BURSTS; i++) {
        if (bursts[i].led < 0) {
            bursts[i].led = random(lo, hi + 1);
            bursts[i].life = 255;
            // Farbpalette: weiß, gelb, gold, cyan
            uint8_t c = random(0, 4);
            switch (c) {
                case 0: bursts[i].r = 255; bursts[i].g = 255; bursts[i].b = 255; break;
                case 1: bursts[i].r = 255; bursts[i].g = 220; bursts[i].b =  60; break;
                case 2: bursts[i].r = 255; bursts[i].g = 160; bursts[i].b =   0; break;
                case 3: bursts[i].r =  60; bursts[i].g = 220; bursts[i].b = 255; break;
            }
            return;
        }
    }
}

void burstStep() {
    for (int i = 0; i < MAX_BURSTS; i++) {
        if (bursts[i].led < 0) continue;
        if (bursts[i].life <= BURST_FADE) {
            bursts[i].led = -1;
            bursts[i].life = 0;
        } else {
            bursts[i].life -= BURST_FADE;
        }
    }
    // zufällig neuen Burst spawnen
    if ((uint16_t)random(0, 1000) < BURST_SPAWN_CHANCE) {
        spawnBurst();
    }
}

void drawAttract() {
    // Feuer als Basis
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) {
        strip.setPixelColor(i, heatColor(heat[i]));
    }
    // Bursts additiv drüberlegen (max pro Kanal)
    for (int i = 0; i < MAX_BURSTS; i++) {
        if (bursts[i].led < 0) continue;
        uint16_t led = bursts[i].led;
        if (led >= NEOPIXEL_COUNT) continue;

        uint8_t bR = ((uint16_t)bursts[i].r * bursts[i].life) / 255;
        uint8_t bG = ((uint16_t)bursts[i].g * bursts[i].life) / 255;
        uint8_t bB = ((uint16_t)bursts[i].b * bursts[i].life) / 255;

        uint32_t existing = strip.getPixelColor(led);
        uint8_t eR = (existing >> 16) & 0xFF;
        uint8_t eG = (existing >>  8) & 0xFF;
        uint8_t eB = (existing      ) & 0xFF;

        strip.setPixelColor(led,
            bR > eR ? bR : eR,
            bG > eG ? bG : eG,
            bB > eB ? bB : eB);
    }
    strip.show();
}

void tickAttractAnimation() {
    fireStep();
    burstStep();
    drawAttract();
}

// Wird einmalig gerufen, wenn state von -1 auf >= 0 wechselt (Initial-QR
// wurde gescannt). Kurzer heller Flash über den ganzen Streifen.
void transitionFlash() {
    Serial.println("TRANSITION: Initial-QR erkannt – Flash!");
    // 6 Frames à ~50 ms, von hell nach dunkel
    const uint8_t steps[6] = { 255, 220, 180, 130, 80, 30 };
    for (int s = 0; s < 6; s++) {
        uint8_t v = steps[s];
        for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) {
            // weiß → gelb → rot über den Verlauf
            uint8_t r = v;
            uint8_t g = (uint16_t)v * (s < 2 ? 255 : (s < 4 ? 180 : 40)) / 255;
            uint8_t b = (uint16_t)v * (s < 2 ? 255 : (s < 4 ?  40 :  0)) / 255;
            strip.setPixelColor(i, r, g, b);
        }
        strip.show();
        delay(50);
    }
    // Heat-Puffer zurücksetzen, damit nach dem Flash nicht sofort wieder
    // ein Feuer-Artefakt sichtbar ist (falls wir je zurückfallen)
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) heat[i] = 0;
}

// -------- Netzwerk --------

void connectWifi() {
    Serial.printf("Verbinde mit WLAN %s...\n", WIFI_SSID);
    WiFi.persistent(false);          // keine Flash-Schreibzugriffe bei jedem Boot
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);     // ESP8266-Stack reconnectet selbstständig
    WiFi.setSleepMode(WIFI_NONE_SLEEP); // Modem-Sleep aus (verhindert Drop-Outs)
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WLAN OK, IP: %s\n", WiFi.localIP().toString().c_str());
        // NTP im Hintergrund starten (nicht blockierend)
        configTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com");
        Serial.println("NTP gestartet");
    } else {
        Serial.println("WLAN fehlgeschlagen, reboot in 5s");
        delay(5000);
        ESP.restart();
    }
}

// Holt den State von MatzEs API (GET API_URL → {"state": N, ...}).
// Plain HTTP – Traefik liefert /state zusätzlich über http aus, damit
// wir uns BearSSL auf dem ESP8266 sparen. Gibt -1 bei Fehler zurück.
int fetchCurrentStation() {
    if (WiFi.status() != WL_CONNECTED) return -1;

    WiFiClient client;
    HTTPClient http;
    if (!http.begin(client, API_URL)) {
        Serial.println("API: http.begin() fehlgeschlagen");
        return -1;
    }
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

    // MatzEs API liefert das Feld "station" (−1 = idle, 0..NUM_STATIONS−1)
    return doc["station"] | -1;
}

// Holt den Status von MatzEs API und übernimmt ihn, falls sich etwas
// geändert hat. Wird im Attract-Modus zeitgesteuert (alle
// ATTRACT_POLL_MS) und im Pendel-Modus am Wendepunkt aufgerufen.
//
// Behandelt drei Übergänge besonders:
//   prev < 0, s < 0  → kein Wechsel, nix tun
//   prev < 0, s >= 0 → Spielstart (Transition-Flash läuft im loop())
//   prev >= 0, s > prev → Bahnhofs-Öffnungs-Animation auf dem neuen
//                         Bahnhof (blockierend, ~1.8 s)
//   prev >= 0, s == NUM_STATIONS-1 → zusätzlich arrivalAnimation()
//   prev >= 0, s < 0 → Server hat idle'd (15 min Timeout), zurück zu Attract
void pollApiAndUpdate() {
    Serial.println("API: check…");
    int s = fetchCurrentStation();
    // Plausibilität: Idle-Sentinel -1 oder 0..NUM_STATIONS-1
    if (s < -1 || s >= (int)NUM_STATIONS) return;
    if (s == currentStation) return;

    const int prev = currentStation;

    // Vorwärts-Übergang zwischen zwei echten Bahnhöfen: Öffnungs-
    // Animation spielen, solange currentStation noch auf dem alten
    // Wert steht (drawBaseline malt den neuen Bahnhof dann noch rot).
    if (prev >= 0 && s > prev) {
        openStationAnimation(s);
    }

    currentStation = s;
    if (s >= 0) {
        Serial.printf("Bahnhof: %d (%s)\n", s, STATION_NAMES[s]);
        resetTrainToCurrent();
        if (s == (int)NUM_STATIONS - 1 && prev != s) {
            Serial.println("Augsburg erreicht!");
            arrivalAnimation();
        }
    } else {
        Serial.println("Idle (Server-Timeout)");
    }
}

// -------- Stabilität: WiFi-Watchdog + Nacht-Neustart --------

static unsigned long s_wifiLostAt = 0;  // 0 = verbunden, sonst Zeitstempel des Verlusts

// Prüft alle 15 s den WiFi-Status. Wenn getrennt: sofort reconnect(); wenn
// länger als 5 Min offline: harter Neustart. Muss zu Beginn jedes loop()-
// Durchlaufs aufgerufen werden.
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
        Serial.println("WiFi getrennt – versuche Reconnect…");
        WiFi.reconnect();
    } else if (now - s_wifiLostAt > 5UL * 60UL * 1000UL) {
        Serial.println("WiFi seit 5 Min weg – Neustart!");
        delay(200);
        ESP.restart();
    }
}

// Prüft einmal pro Minute ob ein täglicher Neustart fällig ist.
// Primär: um 3:00 Uhr MEZ/MESZ (via NTP). Fallback: nach 26 h Laufzeit
// (greift auch ohne NTP-Sync, verhindert Speicherfragmentierung über Zeit).
void checkNightReset() {
    static unsigned long s_lastCheck = 0;
    unsigned long now_ms = millis();
    if (now_ms - s_lastCheck < 60000UL) return;
    s_lastCheck = now_ms;

    // Fallback: nach 26 h Laufzeit neu starten
    if (now_ms > 26UL * 3600UL * 1000UL) {
        Serial.println("26h-Fallback-Neustart");
        delay(200);
        ESP.restart();
    }

    // NTP-basierter Nacht-Neustart um 3:00 Uhr (MEZ/MESZ)
    time_t now_t = time(nullptr);
    struct tm* t = localtime(&now_t);
    // tm_year ist Jahre seit 1900; > 120 = nach 2020 → NTP bereits synced
    if (t->tm_year > 120 && t->tm_hour == 3 && t->tm_min == 0) {
        Serial.println("Nacht-Neustart 3:00 Uhr MEZ");
        delay(200);
        ESP.restart();
    }
}

// -------- Setup / Loop --------

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\nVerstehbahnhof-Cache Firmware");

    // Attract-State initialisieren (freie Burst-Slots, kaltes Feuer)
    for (int i = 0; i < MAX_BURSTS; i++) bursts[i] = { -1, 0, 0, 0, 0 };
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) heat[i] = 0;

    strip.begin();
    strip.setBrightness(NEOPIXEL_BRIGHTNESS);
    // Start schwarz – Attract zeichnet im ersten Frame
    for (uint16_t i = 0; i < NEOPIXEL_COUNT; i++) strip.setPixelColor(i, 0);
    strip.show();

    connectWifi();

    // Einmalige Initialabfrage. Schlägt fehl → currentStation bleibt -1
    // und der Attract-Modus läuft sofort an.
    pollApiAndUpdate();
}

void loop() {
    maintainWifi();
    checkNightReset();

    const unsigned long now = millis();

    if (currentStation < 0) {
        // --- Attract-Modus: Feuer + Feuerwerk ---
        // API muss zeitgesteuert gepollt werden, weil es keinen Wende-
        // punkt gibt, an dem der Pendel-Poll anschlagen könnte.
        static unsigned long lastAttractPoll = 0;
        if (now - lastAttractPoll >= ATTRACT_POLL_MS) {
            lastAttractPoll = now;
            int prev = currentStation;
            pollApiAndUpdate();
            if (prev < 0 && currentStation >= 0) {
                transitionFlash();
            }
        }

        static unsigned long lastAttractFrame = 0;
        if (now - lastAttractFrame >= TRAIN_FRAME_MS) {
            lastAttractFrame = now;
            tickAttractAnimation();
        }
    } else if (currentStation >= (int)NUM_STATIONS - 1) {
        // --- Am Finale: Zug steht still, aber wir müssen trotzdem
        //     regelmäßig pollen, damit wir den Inactivity-Reset
        //     (Server → station=-1) mitbekommen. Sonst bliebe der
        //     ESP auf ewig am Finale hängen.
        static unsigned long lastFinalePoll = 0;
        if (now - lastFinalePoll >= ATTRACT_POLL_MS) {
            lastFinalePoll = now;
            pollApiAndUpdate();
        }

        static unsigned long lastFinaleFrame = 0;
        if (now - lastFinaleFrame >= TRAIN_FRAME_MS) {
            lastFinaleFrame = now;
            tickTrainAnimation();   // zeichnet die statische Finale-Baseline
        }
    } else {
        // --- Normale Pendel-Animation ---
        // API-Poll passiert am Wendepunkt in tickTrainAnimation.
        static unsigned long lastFrame = 0;
        if (now - lastFrame >= TRAIN_FRAME_MS) {
            lastFrame = now;
            tickTrainAnimation();
        }
    }
}
