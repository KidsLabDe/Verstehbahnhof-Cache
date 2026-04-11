# Firmware – Verstehbahnhof-Cache

Firmware für den **Wemos D1 Mini** (ESP8266), der den NeoPixel-Streifen
mit Emmas Reise von Fürstenberg nach Augsburg ansteuert.

## Hardware-Anschluss

| NeoPixel | D1 Mini |
|---|---|
| VCC (+5V) | `5V` |
| GND | `GND` |
| DIN (Data) | `D2` (GPIO4) |

**D4 nicht nehmen** — dort hängt die interne LED, wird beim Boot getoggelt.
Bei Flackern: 470 Ω in die Data-Leitung, 1000 µF Elko an 5V/GND am Streifen-Anfang.
Streifen >30 LEDs → externes 5V-Netzteil, GND gemeinsam.

## Setup

1. [PlatformIO](https://platformio.org/) installieren (VS Code Extension oder CLI)
2. `src/config.h.example` → `src/config.h` kopieren und WLAN + API-URL eintragen
3. Board anschließen, flashen:
   ```
   pio run -t upload
   pio device monitor
   ```

## Wie es läuft

- S2 Mini verbindet sich mit dem WLAN
- Pollt alle 2 s die API (`API_URL` in `config.h`)
- Erwartet JSON: `{ "currentStation": <int>, "totalStations": <int> }`
- Ändert sich `currentStation`, fährt Emma auf dem Streifen animiert
  zum neuen Bahnhof
- Am Ziel (letzter Bahnhof) läuft eine Ankunfts-Animation

Die API selbst baut MatzE parallel — die Firmware ist reiner Client.

## Anpassen

- **Anzahl LEDs / Bahnhöfe:** `NEOPIXEL_COUNT` und `STATION_LEDS[]` in `config.h`
- **Farben:** oben in `main.cpp` (`COLOR_TRAIN`, `COLOR_STATION`, …)
- **Fahrgeschwindigkeit:** `delay(120)` in `animateTrain()`
- **Helligkeit:** `NEOPIXEL_BRIGHTNESS` in `config.h`
