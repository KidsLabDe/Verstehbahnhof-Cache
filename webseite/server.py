import json
import os
import sys
from datetime import datetime, timedelta
from flask import Flask, render_template, jsonify, redirect, request

app = Flask(__name__)

_data_dir = os.environ.get("STATE_DIR", os.path.dirname(__file__))
os.makedirs(_data_dir, exist_ok=True)
STATE_FILE = os.path.join(_data_dir, "state.json")

# Inactivity-Timeouts (in Minuten, via Env konfigurierbar):
#  - INACTIVITY_TIMEOUT_MIN: während des Spiels, bevor der Server auf
#    Idle zurückfällt. Standard 15 min.
#  - ARRIVED_TIMEOUT_MIN: nach dem Gewinn (station == 4, Finale-Screen).
#    Meist kürzer, damit die nächste Gruppe schneller dran ist.
def _int_env(name, default):
    try:
        return int(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default

INACTIVITY_TIMEOUT = timedelta(minutes=_int_env("INACTIVITY_TIMEOUT_MIN", 15))
ARRIVED_TIMEOUT    = timedelta(minutes=_int_env("ARRIVED_TIMEOUT_MIN", 15))

# -------- Logging --------
#
# Daily rotating log files in LOG_DIR/YYYY-MM-DD.log. Eigene kleine
# Implementierung, weil wir _wirklich_ Dateien im Format yyyy-mm-dd.log
# haben wollen (nicht cache.log.2026-04-11 wie bei TimedRotatingFileHandler).
# Gibt den Output zusätzlich auf stdout aus, damit `docker compose logs`
# live mitliest.

LOG_DIR = os.environ.get("LOG_DIR", os.path.join(_data_dir, "log"))
os.makedirs(LOG_DIR, exist_ok=True)
LOGBOOK_FILE = os.path.join(LOG_DIR, "logbook.jsonl")


def log_event(level, event, **fields):
    ts = datetime.now().isoformat(timespec="seconds")
    extras = " ".join(f"{k}={json.dumps(v, ensure_ascii=False)}"
                      for k, v in fields.items())
    line = f"{ts} {level:5s} {event}"
    if extras:
        line += " " + extras
    line += "\n"

    try:
        path = os.path.join(LOG_DIR, datetime.now().strftime("%Y-%m-%d") + ".log")
        with open(path, "a", encoding="utf-8") as f:
            f.write(line)
    except Exception as exc:  # Log-Failures dürfen den Request nicht killen
        print(f"[log-error] {exc}", file=sys.stderr, flush=True)

    print(line, end="", flush=True)

# Bahnhof-Indizes (müssen zur Firmware-Reihenfolge passen)
IDLE = -1
STATION_FURSTENBERG = 0
STATION_BERLIN = 1
STATION_LEIPZIG = 2
STATION_NURNBERG = 3
STATION_AUGSBURG = 4
NUM_STATIONS = 5

STATION_NAMES = {
    -1: "idle",
    0: "furstenberg",
    1: "berlin",
    2: "leipzig",
    3: "nurnberg",
    4: "augsburg",
}


# -------- State-Persistenz --------

def _fresh():
    return {
        "station": IDLE,
        "task_done": False,
        "last_activity": datetime.now().isoformat(),
    }


def load_state():
    if not os.path.exists(STATE_FILE):
        return _fresh()
    with open(STATE_FILE) as f:
        data = json.load(f)
    # Migration vom alten Schema {"state": N}: wenn nur das alte
    # "state"-Feld vorliegt, wird der Spielstand komplett zurück-
    # gesetzt. Das alte state=0 war "Spiel läuft, aber nichts
    # gescannt" – im neuen Schema nicht 1:1 mappbar, daher lieber
    # sauber neu starten, damit niemand in einem "zu früh zurück"-
    # Zombie-Zustand hängen bleibt.
    if "station" not in data:
        fresh = _fresh()
        save_state(fresh)
        return fresh
    data.setdefault("task_done", False)
    data.setdefault("last_activity", datetime.now().isoformat())
    return data


def save_state(data):
    with open(STATE_FILE, "w") as f:
        json.dump(data, f)


def get_state():
    data = load_state()
    last = datetime.fromisoformat(data["last_activity"])
    # Am Finale gilt ein ggf. kürzerer Timeout, damit die nächste Gruppe
    # nicht ewig auf den Reset warten muss.
    at_finale = data.get("station") == NUM_STATIONS - 1
    timeout = ARRIVED_TIMEOUT if at_finale else INACTIVITY_TIMEOUT
    if datetime.now() - last > timeout:
        log_event("INFO", "inactivity_reset",
                  prev_station=data.get("station"),
                  prev_task_done=data.get("task_done"),
                  at_finale=at_finale,
                  timeout_min=int(timeout.total_seconds() / 60))
        data = _fresh()
        save_state(data)
    return data


def touch(data=None):
    if data is None:
        data = get_state()
    data["last_activity"] = datetime.now().isoformat()
    save_state(data)
    return data


# -------- Game-Logik --------

def mark_task_done(required_station):
    """Markiert die Task am required_station als erledigt. Wenn der
    Spieler nicht dort ist, passiert nichts außer einem touch."""
    data = get_state()
    matched = (data["station"] == required_station and not data["task_done"])
    if matched:
        data["task_done"] = True
    data["last_activity"] = datetime.now().isoformat()
    save_state(data)
    log_event("INFO", "task_scan",
              required=required_station,
              current=data["station"],
              matched=matched,
              task_done=data["task_done"])
    return data


def scan_start_qr_impl():
    """Der Start-QR hat mehrere Bedeutungen, je nach aktuellem Zustand:

    - Idle (niemand hat gespielt): Spiel starten, Station → Fürstenberg
    - Task am aktuellen Bahnhof erledigt: nächsten Bahnhof freischalten
    - Task noch offen: gar nix ändern (der Spieler muss erst die Aufgabe
      finden)
    - Am Ziel angekommen: einfach touch

    Returns (data, kind) mit kind ∈ {started, advanced, arrived, pending}.
    """
    data = get_state()

    if data["station"] == IDLE:
        data = {
            "station": STATION_FURSTENBERG,
            "task_done": False,
            "last_activity": datetime.now().isoformat(),
        }
        save_state(data)
        return data, "started"

    if data["task_done"] and data["station"] < NUM_STATIONS - 1:
        data = {
            "station": data["station"] + 1,
            "task_done": False,
            "last_activity": datetime.now().isoformat(),
        }
        save_state(data)
        return data, "advanced"

    if data["station"] == NUM_STATIONS - 1:
        data = touch(data)
        return data, "arrived"

    # Task noch offen, aktueller Bahnhof
    data = touch(data)
    return data, "pending"


def scan_start_qr():
    data, kind = scan_start_qr_impl()
    log_event("INFO", "start_qr_scan", kind=kind,
              station=data["station"], task_done=data["task_done"])
    return data, kind


# -------- Routes --------

@app.route("/")
def index():
    touch()
    return render_template("index.html")


@app.route("/aufgabe_st4rt_v9p")
def aufgabe_start():
    data, kind = scan_start_qr()
    return render_template(
        "aufgabe_st4rt_v9p.html",
        kind=kind,
        station=data["station"],
        num_stations=NUM_STATIONS,
    )


@app.route("/aufgabe1")
def aufgabe1():
    mark_task_done(STATION_FURSTENBERG)
    return render_template("aufgabe1.html")


@app.route("/aufgabe_w3rkst4tt_k9p")
def aufgabe_repair():
    data = get_state()
    # Easter Egg: Wer ohne Spielstart hier reinstolpert, wird freundlich
    # auf die echte Repair-Café-Seite umgeleitet.
    if data["station"] < STATION_BERLIN:
        return redirect("https://reparaturbahnhof.de/")
    mark_task_done(STATION_BERLIN)
    return render_template("aufgabe_w3rkst4tt_k9p.html")


@app.route("/aufgabe_g0ldst3in_m7x")
def aufgabe_gold():
    mark_task_done(STATION_LEIPZIG)
    return render_template("aufgabe_g0ldst3in_m7x.html")


@app.route("/aufgabe_l0r3nf4hrt_b3q")
def aufgabe_lore():
    mark_task_done(STATION_NURNBERG)
    return render_template("aufgabe_l0r3nf4hrt_b3q.html")


@app.route("/reset")
def reset():
    # Bestätigungs-Zwischenseite; der eigentliche Reset passiert erst
    # auf /reset/confirm, damit versehentliches Klicken oder Prefetching
    # nicht den Spielstand zerschießt.
    return render_template("reset.html")


@app.route("/reset/confirm")
def reset_confirm():
    before = load_state()
    fresh = _fresh()
    save_state(fresh)
    log_event("INFO", "manual_reset",
              prev_station=before.get("station"),
              prev_task_done=before.get("task_done"))
    return render_template("reset_done.html")


@app.route("/logbook", methods=["POST"])
def logbook_submit():
    name = (request.form.get("name") or "").strip()[:100]
    comment = (request.form.get("comment") or "").strip()[:2000]
    if not name or not comment:
        log_event("WARN", "logbook_rejected",
                  reason="empty", has_name=bool(name), has_comment=bool(comment))
        return redirect("/aufgabe_st4rt_v9p")

    entry = {
        "timestamp": datetime.now().isoformat(timespec="seconds"),
        "name": name,
        "comment": comment,
    }
    try:
        with open(LOGBOOK_FILE, "a", encoding="utf-8") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
    except Exception as exc:
        log_event("ERROR", "logbook_write_failed", error=str(exc))

    log_event("INFO", "logbook_entry",
              name=name, comment_len=len(comment))
    return render_template("logbook_thanks.html", name=name, comment=comment)


@app.route("/state")
def state():
    data = get_state()
    return jsonify({
        "station": data["station"],
        "task_done": data["task_done"],
        "station_name": STATION_NAMES.get(data["station"], "unknown"),
        "last_activity": data["last_activity"],
    })


log_event("INFO", "server_start",
          state_dir=_data_dir, log_dir=LOG_DIR,
          inactivity_min=int(INACTIVITY_TIMEOUT.total_seconds() / 60),
          arrived_min=int(ARRIVED_TIMEOUT.total_seconds() / 60))


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
