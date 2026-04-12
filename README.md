# Verstehbahnhof-Cache

Ein interaktiver GeoCache für den [Verstehbahnhof](https://verstehbahnhof.de/)
in Fürstenberg/Havel. Besucher:innen helfen Jim Knopf, seine Lok Emma
von Fürstenberg zurück zur Augsburger Puppenkiste zu bringen — indem
sie QR-Codes am Verstehbahnhof scannen und dabei auf einem LED-Streifen
zusehen, wie Emma Bahnhof für Bahnhof nach Hause rollt.

## Die Geschichte

Jim Knopf, Lukas der Lokomotivführer und die kleine Lok **Emma** sind
auf dem Heimweg nach **Augsburg** gestrandet. Das Schloss an Emmas
Feuerbüchse klemmt, der Goldstein ist verloren, und Emma ist zu
schwer zum Anschieben. Ohne Hilfe kommen die drei nicht weiter.

An der Strecke vor dem Exponat flackert das Lagerfeuer (LED-Streifen
im Attract-Modus) — es lockt Besucher:innen an. Wer den
Start-QR-Code scannt, startet das Abenteuer: Emma muss über vier
Stationen wieder fit gemacht werden. Am Ende rollt sie in Augsburg ein.

## Wie das Spiel abläuft

Die Spieler:innen begegnen pro Bahnhof **zwei** QR-Codes:

1. **Aufgaben-QR** — irgendwo am Verstehbahnhof versteckt (am Lok-Bild,
   am Repair-Café-Plakat, am Goldstein, an der Lore). Beim Scannen wird
   die Aufgabe am aktuellen Bahnhof als erledigt markiert und die
   Webseite sagt: "Geh zurück zum Start-QR".
2. **Start-QR** — neben Emma am LED-Streifen. Dasselbe physische
   Exponat, dieselbe URL. Beim ersten Scan startet das Spiel, bei jedem
   weiteren Scan wird der nächste Bahnhof freigeschaltet (vorausgesetzt
   die Aufgabe wurde gelöst). Wird der Start-QR zu früh gescannt, sagt
   die Webseite "du bist zu früh zurück".

```
Idle (Lagerfeuer)
  └─ Start-QR → Fürstenberg, Attract endet, Pendelei 0↔1 beginnt
        └─ Lok-QR → task_done
              └─ Start-QR → Bahnhof Berlin öffnet sich, Pendelei 1↔2
                    └─ Repair-Café-QR → task_done
                          └─ Start-QR → Bahnhof Leipzig öffnet sich
                                └─ Goldstein-QR → task_done
                                      └─ Start-QR → Nürnberg öffnet sich
                                            └─ Lore-QR → task_done
                                                  └─ Start-QR → Augsburg + Finale
```

Am Finale darf die Gruppe sich mit Namen + Gruß ins **Logbuch**
eintragen. Nach dem Timeout (standardmäßig 5 min nach Gewinn,
15 min während des Spiels) fällt alles zurück auf Idle.

Die fünf Bahnhöfe auf dem LED-Streifen: **Fürstenberg → Berlin →
Leipzig → Nürnberg → Augsburg**. Sie sind gleichmäßig über den
Streifen verteilt, die Firmware rechnet die LED-Positionen automatisch
aus `NEOPIXEL_COUNT` aus.

## Architektur

```
┌──────────────┐           ┌──────────────────┐           ┌────────────┐
│  Smartphone  │  scan QR  │  verstehbahnhof. │   GET     │  D1 Mini   │
│  Besucher:in │ ────────▶ │  kidslab.de      │ ◀──────── │  Lite      │
│              │           │  (Flask+Gunicorn)│  /state   │  (Firmware)│
└──────────────┘           └─────────┬────────┘           └─────┬──────┘
                                     │                          │
                                     ▼                          ▼
                             ┌──────────────┐          ┌─────────────────┐
                             │ state.json + │          │ NeoPixel-Streif │
                             │ log/*.log    │          │ (Feuer, Zug,    │
                             │ (Volumes)    │          │  Waggons)       │
                             └──────────────┘          └─────────────────┘
```

- **Webseite** (`webseite/`): Flask-App mit Gunicorn, rendert Story-
  Seiten pro QR-Scan, verwaltet den zentralen Spielstand, schreibt
  Logs und Logbuch-Einträge. Läuft hinter Traefik unter
  `verstehbahnhof.kidslab.de`.
- **Firmware** (`firmware/`): PlatformIO-Projekt für den **Wemos D1
  Mini Lite** (ESP8285). Pollt alle 10 s bzw. am Pendel-Wendepunkt
  die `/state`-API und animiert den NeoPixel-Streifen.
- **QR-Codes** (`qr-codes/`): Fünf druckbare PNGs für die Stationen.

## Hardware

- **Wemos D1 Mini Lite** (ESP8285, 1 MB Flash)
- **NeoPixel-LED-Streifen** (WS2812B), empfohlen 30+ LEDs
- Anschluss:
  | NeoPixel | D1 Mini |
  | --- | --- |
  | VCC (+5V) | `5V` |
  | GND | `GND` |
  | DIN (Data) | `D2` (GPIO4) |
- Bei Flackern: 470 Ω in die Data-Leitung, 1000 µF Elko am Streifen-
  Anfang zwischen 5V und GND. Streifen >30 LEDs → externes 5V-Netzteil.
- Siehe `firmware/README.md` für Details.

## Firmware-Animationen

| Zustand | Animation |
|---|---|
| **Idle** (`station=-1`) | Fire2012-Feuer auf dem ganzen Streifen (schwarz→rot→gelb→weiß), alle 1-3 s ein heller Feuerwerks-Burst an zufälliger Position, der ausfadet |
| **Spielstart** (`-1 → 0`) | Einmaliger Transition-Flash (weiß → gelb → rot, ~300 ms) über den ganzen Streifen |
| **Pendelei** (`station >= 0, < 4`) | Erreichte Bahnhöfe grün, offene rot, befahrene Strecke dim weiß. Emma (blau) + 3 Waggons pendeln zwischen aktuellem und nächstem Bahnhof (kehren vor dem roten Bahnhof um) |
| **Bahnhof öffnet sich** (`N → N+1`) | Neuer Bahnhof blinkt 3× rot, fadet aus, fadet grün ein (~1.8 s, blockierend). Danach normale Pendelei |
| **Finale** (`station=4`) | Öffnungs-Animation + Ankunfts-Party (6× Ganz-Streifen-Grün-Flash) |
| **Server-Timeout** (`N → -1`) | Firmware merkt es beim nächsten Poll, fällt in Idle (Feuer+Feuerwerk) zurück |

API-Polling:
- **Im Idle**: alle **10 s** (`ATTRACT_POLL_MS`)
- **Im Spiel**: nur am Wendepunkt der Pendelei (vorm nächsten roten Bahnhof)

## Server / Game-Logik

- `state.json` (`{station, task_done, last_activity}`) — persistent im
  `state`-Volume
- Start-QR hat vier Reaktionen: `started` / `advanced` / `pending` / `arrived`
- Aufgaben-QRs setzen nur `task_done=true` am passenden Bahnhof
- `/reset` zeigt Bestätigungs-Seite, `/reset/confirm` setzt zurück
- `/logbook` (POST) nimmt Logbuch-Einträge an, speichert in
  `log/logbook.jsonl`
- Strukturiertes Logging in `log/YYYY-MM-DD.log` (und parallel auf
  stdout für `docker compose logs`)

## Konfiguration (`compose.yml`)

| Env-Variable | Default | Bedeutung |
|---|---|---|
| `STATE_DIR` | `/app/state` | Ort der state.json |
| `LOG_DIR` | `/app/log` | Ort der Log-Dateien + logbook.jsonl |
| `INACTIVITY_TIMEOUT_MIN` | `15` | Timeout in Minuten während des Spiels |
| `ARRIVED_TIMEOUT_MIN` | `5` | Timeout in Minuten nach dem Finale |

Alle Werte stehen direkt in `compose.yml`. Zum Ändern Wert anpassen
und `docker compose up -d` neu aufrufen — Rebuild nicht nötig.

Firmware-Config: `firmware/src/config.h` (gitignored), Vorlage in
`firmware/src/config.h.example`. Die wichtigsten Einstellungen:

| Define | Default | Bedeutung |
|---|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | — | WLAN-Zugang |
| `API_URL` | `http://verstehbahnhof.kidslab.de/state` | /state-Endpunkt, plain HTTP über Traefik-Sonderroute |
| `API_TIMEOUT_MS` | `1500` | HTTP-Timeout |
| `NEOPIXEL_PIN` | `D2` | Data-Pin |
| `NEOPIXEL_COUNT` | `30` | LEDs insgesamt |
| `NEOPIXEL_BRIGHTNESS` | `80` | 0..255 |
| `TRAIN_FRAME_MS` | `18` | Pendel-Geschwindigkeit (ms/LED) |
| `NUM_WAGONS` | `3` | Waggons hinter der Lok |
| `ATTRACT_POLL_MS` | `10000` | Idle-Poll-Intervall |
| `FIRE_COOLING` / `FIRE_SPARKING` | `55` / `120` | Fire2012-Tuning |

## Deployment

**Auf dem Server:**
```bash
git pull
docker compose up -d --build
```

Traefik-Labels sind in `compose.yml` drin, das externe `proxy`-Netz
muss existieren und Traefik im selben Netz laufen. Die Domain
`verstehbahnhof.kidslab.de` muss auf den Host zeigen.

`/state` wird zusätzlich über **plain HTTP** ausgeliefert (ohne
HTTPS-Redirect), damit der ESP8266 sich keinen BearSSL-Handshake
zumuten muss. Alle anderen Pfade werden auf HTTPS umgeleitet.

**Firmware flashen:**
```bash
cd firmware
cp src/config.h.example src/config.h    # einmalig, dann editieren
pio run -t upload
pio device monitor                       # optional: live-log
```

## Inspektion auf dem Host

Die Logs liegen per Bind-Mount direkt neben `compose.yml` im Ordner
`logs/` — da kann man sie ohne `docker compose exec` lesen und backuppen:

```bash
# Tageslog live mitlesen
tail -f logs/$(date +%F).log

# Alle Logbuch-Einträge
cat logs/logbook.jsonl | jq .

# Spielstand ansehen (steckt in einem Named Volume, daher via exec)
docker compose exec webseite cat /app/state/state.json

# State komplett zurücksetzen (vor Event, Testlauf, etc.)
docker compose exec webseite rm -f /app/state/state.json
```

## Wichtige Dateien im Repo

```
compose.yml               Docker Compose + Traefik-Labels
webseite/                 Flask-Server
  server.py                 Game-Logik, Routes, Logging
  Dockerfile                Gunicorn-Runtime
  templates/                Jinja2-Templates pro State/Route
firmware/                 PlatformIO-Projekt für D1 Mini Lite
  src/main.cpp              Fire2012, Pendelei, Öffnungs-Animationen
  src/config.h.example      Pins, WLAN-Platzhalter, Tuning
  platformio.ini            Board + Libraries
qr-codes/                 Druckbare QR-Codes für die 5 Stationen
README.md                 ← du bist hier
TESTING.md                URLs, curl-Flows, Debug-Tipps
```

## Über den Verstehbahnhof

Der [Verstehbahnhof](https://verstehbahnhof.de/) in Fürstenberg/Havel
ist ein Maker- und Lernort für Kinder und Jugendliche rund um Technik,
Programmieren und Eisenbahn. Dieses Projekt ist ein kleiner
GeoCache-Beitrag für Besucher:innen des Verstehbahnhofs.

## Lizenz

tbd
