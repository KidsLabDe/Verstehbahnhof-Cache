# Testing – Verstehbahnhof-Cache

URLs und Debug-Hilfen zum Testen des Spielflusses. Dieselben URLs landen
auch auf den gedruckten QR-Codes ams Verstehbahnhof.

## QR-Codes in Scan-Reihenfolge

| Schritt | URL | Effekt im Server | Effekt auf der Firmware |
|---|---|---|---|
| 1. Initial / Start-QR | <http://verstehbahnhof.kidslab.de/aufgabe_st4rt_v9p> | Je nach Zustand: Spielstart / Bahnhof öffnen / "zu früh" / Finale | idle→Fürstenberg: Transition-Flash, dann Pendel 0↔1. Bahnhof-Öffnen: Öffnungs-Animation (rot→fade→grün), dann Pendel N↔N+1 |
| 2. Lok / Schloss-QR | <http://verstehbahnhof.kidslab.de/aufgabe1> | `task_done=true` an Fürstenberg | keine direkte Änderung – wartet auf den nächsten Start-QR-Scan |
| 3. Repair-Café-QR | <http://verstehbahnhof.kidslab.de/aufgabe_w3rkst4tt_k9p> | `task_done=true` an Berlin. Wenn der Spieler noch gar nicht gestartet hat: freundlicher Redirect auf reparaturbahnhof.de | keine direkte Änderung |
| 4. Goldstein-QR | <http://verstehbahnhof.kidslab.de/aufgabe_g0ldst3in_m7x> | `task_done=true` an Leipzig | keine direkte Änderung |
| 5. Lore-QR | <http://verstehbahnhof.kidslab.de/aufgabe_l0r3nf4hrt_b3q> | `task_done=true` an Nürnberg | keine direkte Änderung |

Der Start-QR wird also **fünfmal** gescannt: einmal zum Starten,
viermal zum Freischalten der nächsten Bahnhöfe.

## Debug-Endpunkte

| URL | Zweck |
|---|---|
| <http://verstehbahnhof.kidslab.de/state> | JSON `{station, task_done, station_name, last_activity}` – genau das, was die Firmware alle 10 s im Idle bzw. am Pendel-Wendepunkt pollt. Traefik serviert diesen Pfad plain HTTP (kein Redirect), damit der ESP8266 sich keinen TLS-Handshake zumuten muss. |
| <https://verstehbahnhof.kidslab.de/> | Story-Intro (`index.html`). Touch nur der `last_activity`, kein State-Wechsel. |

## Typischer Test-Flow mit curl

Jeweils nach dem Scan die `/state`-Response kontrollieren. Reihenfolge:

```bash
# 0. State auf dem Server komplett zurücksetzen
docker compose exec webseite rm -f /app/state/state.json

curl http://verstehbahnhof.kidslab.de/state
# → {"station":-1,"task_done":false,"station_name":"idle",...}
# Firmware: Feuer + Feuerwerks-Bursts, kein Zug

# 1. Initial-QR scannen
curl -s http://verstehbahnhof.kidslab.de/aufgabe_st4rt_v9p > /dev/null
curl http://verstehbahnhof.kidslab.de/state
# → {"station":0,"task_done":false,"station_name":"furstenberg",...}
# Firmware: Transition-Flash, dann Pendel Fürstenberg↔Berlin

# 2. Lok/Schloss-QR scannen
curl -s http://verstehbahnhof.kidslab.de/aufgabe1 > /dev/null
curl http://verstehbahnhof.kidslab.de/state
# → {"station":0,"task_done":true,...}
# Firmware: unverändert (Pendel läuft weiter)

# 3. Zurück zum Start-QR → Berlin wird freigeschaltet
curl -s http://verstehbahnhof.kidslab.de/aufgabe_st4rt_v9p > /dev/null
curl http://verstehbahnhof.kidslab.de/state
# → {"station":1,"task_done":false,"station_name":"berlin",...}
# Firmware: Öffnungs-Animation auf Bahnhof 1, dann Pendel Berlin↔Leipzig

# 4. Repair-Café-QR → zurück zum Start → Leipzig öffnet sich → ...
curl -s http://verstehbahnhof.kidslab.de/aufgabe_w3rkst4tt_k9p > /dev/null
curl -s http://verstehbahnhof.kidslab.de/aufgabe_st4rt_v9p > /dev/null
curl -s http://verstehbahnhof.kidslab.de/aufgabe_g0ldst3in_m7x > /dev/null
curl -s http://verstehbahnhof.kidslab.de/aufgabe_st4rt_v9p > /dev/null
curl -s http://verstehbahnhof.kidslab.de/aufgabe_l0r3nf4hrt_b3q > /dev/null
curl -s http://verstehbahnhof.kidslab.de/aufgabe_st4rt_v9p > /dev/null
curl http://verstehbahnhof.kidslab.de/state
# → {"station":4,"task_done":false,"station_name":"augsburg",...}
# Firmware: Öffnungs-Animation auf Augsburg + Ankunfts-Party
```

## Edge Cases / Easter Eggs

- **Ohne Spielstart am Repair-Café scannen** (`station < 1`) → Server
  leitet freundlich auf <https://reparaturbahnhof.de/> weiter. Der State
  bleibt unverändert.
- **Start-QR zu früh scannen** (Task noch offen am aktuellen Bahnhof) →
  Template `kind="pending"`, Story-Hinweis "du bist zu früh zurück", kein
  State-Wechsel, Firmware unverändert.
- **Task-QR im falschen State** (z. B. `/aufgabe1` wenn schon bei
  `station=2`) → `mark_task_done` ist no-op, Template rendert trotzdem.
  Ignorieren.
- **15 Minuten keine Interaktion** → Server setzt `station=-1`,
  `task_done=false`. Firmware fällt innerhalb von 10 s (nächster
  Attract-Poll) zurück in Feuer+Feuerwerk.

## State direkt auf dem Server inspizieren

```bash
docker compose exec webseite cat /app/state/state.json
# {"station": 2, "task_done": false, "last_activity": "2026-04-11T..."}
```

Manuell setzen (für Demos):

```bash
docker compose exec webseite sh -c \
  'echo "{\"station\":3,\"task_done\":true,\"last_activity\":\"$(date -Iseconds)\"}" > /app/state/state.json'
```

Die Firmware übernimmt das spätestens beim nächsten Poll (10 s im Idle,
am nächsten Pendel-Wendepunkt im Spiel).

## Firmware-Live-Monitoring

```bash
cd firmware
pio device monitor
```

Erwartete Log-Zeilen:
- `Verstehbahnhof-Cache Firmware` beim Boot
- `WLAN OK, IP: 192.168.x.x`
- `API: check…` bei jedem Poll
- `Bahnhof: 2 (Goldstein (Leipzig))` beim State-Wechsel
- `TRANSITION: Initial-QR erkannt – Flash!` beim Spielstart
- `Augsburg erreicht!` beim Finale
- `Idle (Server-Timeout)` wenn der Server auf `station=-1` zurückfällt
