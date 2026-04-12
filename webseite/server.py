import json
import os
import sys
from datetime import datetime, timedelta
from flask import Flask, render_template, jsonify, redirect, request

app = Flask(__name__)


@app.after_request
def no_cache(response):
    # Alle Seiten sind dynamisch und verändern Server-State beim Aufruf.
    # Ohne diesen Header lädt der Browser (z.B. der QR-Scanner) dieselbe
    # URL aus dem Cache – scan_start_qr() wird nie aufgerufen und der
    # Spielstand friert ein.
    response.headers["Cache-Control"] = "no-store, no-cache, must-revalidate, max-age=0"
    response.headers["Pragma"] = "no-cache"
    response.headers["Expires"] = "0"
    return response


# -------- Konfiguration --------

def _int_env(name, default):
    try:
        return int(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


_data_dir = os.environ.get("STATE_DIR", os.path.dirname(__file__))
os.makedirs(_data_dir, exist_ok=True)

LOG_DIR = os.environ.get("LOG_DIR", os.path.join(_data_dir, "log"))
os.makedirs(LOG_DIR, exist_ok=True)


# -------- Logging --------
#
# Daily rotating log files in LOG_DIR/YYYY-MM-DD.log.
# Jede Zeile enthält game="vb" oder game="aug", damit beide Caches
# in derselben Datei erkennbar sind.
# Gibt den Output zusätzlich auf stdout aus, damit `docker compose logs`
# live mitliest.

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
    except Exception as exc:
        print(f"[log-error] {exc}", file=sys.stderr, flush=True)

    print(line, end="", flush=True)


# -------- Hilfsfunktionen --------

def _duration_sec(data):
    """Spieldauer in Sekunden aus started_at/finished_at, oder None."""
    s = data.get("started_at")
    f = data.get("finished_at")
    if not s or not f:
        return None
    try:
        return int((datetime.fromisoformat(f) -
                    datetime.fromisoformat(s)).total_seconds())
    except (ValueError, TypeError):
        return None


def _format_duration(sec):
    """Formatiert Sekunden als menschenlesbare Dauer."""
    if sec is None or sec < 0:
        return "—"
    mins, secs = divmod(int(sec), 60)
    if mins >= 60:
        h, m = divmod(mins, 60)
        return f"{h}h {m}m {secs}s"
    return f"{mins}m {secs:02d}s"


# -------- Game-Klasse --------
#
# Kapselt State-Persistenz, Spiellogik und Logging für einen Cache.
# Beide Caches (Verstehbahnhof und Augsburg) sind Instanzen dieser Klasse.

class Game:
    def __init__(self, game_id, state_file, num_stations, station_names,
                 logbook_file, inactivity_timeout, arrived_timeout):
        self.gid           = game_id
        self.state_file    = state_file
        self.N             = num_stations
        self.station_names = station_names   # {-1: "idle", 0: "...", ...}
        self.logbook_file  = logbook_file
        self.inactivity_to = inactivity_timeout
        self.arrived_to    = arrived_timeout

    # ---- Logging ----

    def log(self, level, event, **fields):
        log_event(level, event, game=self.gid, **fields)

    # ---- State-Persistenz ----

    @staticmethod
    def _fresh():
        return {
            "station": -1,
            "task_done": False,
            "last_activity": datetime.now().isoformat(),
            "started_at": None,
            "finished_at": None,
        }

    def load_state(self):
        if not os.path.exists(self.state_file):
            return self._fresh()
        try:
            with open(self.state_file) as f:
                data = json.load(f)
        except (json.JSONDecodeError, OSError):
            return self._fresh()
        if "station" not in data:
            fresh = self._fresh()
            self.save_state(fresh)
            return fresh
        data.setdefault("task_done", False)
        data.setdefault("last_activity", datetime.now().isoformat())
        data.setdefault("started_at", None)
        data.setdefault("finished_at", None)
        return data

    def save_state(self, data):
        with open(self.state_file, "w") as f:
            json.dump(data, f)

    def get_state(self):
        """Gibt aktuellen State zurück; löst ggf. Inactivity-Reset aus."""
        data = self.load_state()
        last = datetime.fromisoformat(data["last_activity"])
        at_finale = data.get("station") == self.N - 1
        timeout = self.arrived_to if at_finale else self.inactivity_to
        if datetime.now() - last > timeout:
            self.log("INFO", "inactivity_reset",
                     prev_station=data.get("station"),
                     prev_task_done=data.get("task_done"),
                     at_finale=at_finale,
                     timeout_min=int(timeout.total_seconds() / 60))
            data = self._fresh()
            self.save_state(data)
        return data

    def touch(self, data=None):
        if data is None:
            data = self.get_state()
        data["last_activity"] = datetime.now().isoformat()
        self.save_state(data)
        return data

    # ---- Spiellogik ----

    def mark_task_done(self, required_station):
        """Markiert die Aufgabe an required_station als erledigt."""
        data = self.get_state()
        matched = (data["station"] == required_station and not data["task_done"])
        if matched:
            data["task_done"] = True
        data["last_activity"] = datetime.now().isoformat()
        self.save_state(data)
        self.log("INFO", "task_scan",
                 required=required_station,
                 current=data["station"],
                 matched=matched,
                 task_done=data["task_done"])
        return data

    def scan_start_qr(self):
        """Wertet den Start-QR aus. Gibt (data, kind) zurück.
        kind ∈ {started, advanced, pending, arrived}"""
        data = self.get_state()
        now_iso = datetime.now().isoformat()

        if data["station"] == -1:
            data = {
                "station": 0,
                "task_done": False,
                "last_activity": now_iso,
                "started_at": now_iso,
                "finished_at": None,
            }
            self.save_state(data)
            self.log("INFO", "start_qr_scan", kind="started",
                     station=0, task_done=False)
            return data, "started"

        if data["task_done"] and data["station"] < self.N - 1:
            new_station = data["station"] + 1
            new_data = {
                "station": new_station,
                "task_done": False,
                "last_activity": now_iso,
                "started_at": data.get("started_at"),
                "finished_at": data.get("finished_at"),
            }
            if new_station == self.N - 1:
                new_data["finished_at"] = now_iso
                self.log("INFO", "game_finished",
                         started_at=new_data["started_at"],
                         finished_at=new_data["finished_at"],
                         duration_sec=_duration_sec(new_data))
            self.save_state(new_data)
            self.log("INFO", "start_qr_scan", kind="advanced",
                     station=new_station, task_done=False)
            return new_data, "advanced"

        if data["station"] == self.N - 1:
            data = self.touch(data)
            self.log("INFO", "start_qr_scan", kind="arrived",
                     station=data["station"], task_done=data["task_done"])
            return data, "arrived"

        # Task noch offen
        data = self.touch(data)
        self.log("INFO", "start_qr_scan", kind="pending",
                 station=data["station"], task_done=data["task_done"])
        return data, "pending"

    # ---- Logbuch ----

    def read_logbook(self):
        entries = []
        if not os.path.exists(self.logbook_file):
            return entries
        try:
            with open(self.logbook_file, encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if line:
                        try:
                            entries.append(json.loads(line))
                        except json.JSONDecodeError:
                            pass
        except Exception as exc:
            self.log("ERROR", "logbook_read_failed", error=str(exc))
        entries.reverse()
        return entries

    def write_logbook_entry(self, name, comment):
        data_now = self.get_state()
        duration_sec = _duration_sec(data_now)
        entry = {
            "timestamp": datetime.now().isoformat(timespec="seconds"),
            "name": name,
            "comment": comment,
            "duration_sec": duration_sec,
            "started_at": data_now.get("started_at"),
            "finished_at": data_now.get("finished_at"),
        }
        try:
            with open(self.logbook_file, "a", encoding="utf-8") as f:
                f.write(json.dumps(entry, ensure_ascii=False) + "\n")
        except Exception as exc:
            self.log("ERROR", "logbook_write_failed", error=str(exc))
            return None
        self.log("INFO", "logbook_entry",
                 name=name, comment_len=len(comment), duration_sec=duration_sec)
        return entry


# -------- Spiel-Instanzen --------

# --- Verstehbahnhof (Fürstenberg) ---
_VB_STATE_FILE = os.path.join(_data_dir, "state.json")
_VB_LOGBOOK    = os.path.join(LOG_DIR, "logbook.jsonl")

vb = Game(
    game_id      = "vb",
    state_file   = _VB_STATE_FILE,
    num_stations = 5,
    station_names = {
        -1: "idle",
        0: "furstenberg",
        1: "berlin",
        2: "leipzig",
        3: "nurnberg",
        4: "augsburg",
    },
    logbook_file       = _VB_LOGBOOK,
    inactivity_timeout = timedelta(minutes=_int_env("INACTIVITY_TIMEOUT_MIN", 15)),
    arrived_timeout    = timedelta(minutes=_int_env("ARRIVED_TIMEOUT_MIN", 5)),
)

# --- Augsburg Cache ---
_AUG_STATE_FILE = os.path.join(_data_dir, "augsburg_state.json")
_AUG_LOGBOOK    = os.path.join(LOG_DIR, "logbook_augsburg.jsonl")
_AUG_N          = _int_env("AUG_NUM_STATIONS", 3)

aug = Game(
    game_id      = "aug",
    state_file   = _AUG_STATE_FILE,
    num_stations = _AUG_N,
    station_names = {i: (f"station_{i}" if i >= 0 else "idle")
                     for i in range(-1, _AUG_N)},
    logbook_file       = _AUG_LOGBOOK,
    inactivity_timeout = timedelta(minutes=_int_env("AUG_INACTIVITY_TIMEOUT_MIN", 15)),
    arrived_timeout    = timedelta(minutes=_int_env("AUG_ARRIVED_TIMEOUT_MIN", 5)),
)


# -------- Verstehbahnhof-Routen --------

@app.route("/")
def index():
    vb.touch()
    return render_template("index.html")


@app.route("/aufgabe_st4rt_v9p")
def aufgabe_start():
    data, kind = vb.scan_start_qr()
    return render_template(
        "aufgabe_st4rt_v9p.html",
        kind=kind,
        station=data["station"],
        num_stations=vb.N,
    )


@app.route("/aufgabe1")
def aufgabe1():
    vb.mark_task_done(0)
    return render_template("aufgabe1.html")


@app.route("/aufgabe_w3rkst4tt_k9p")
def aufgabe_repair():
    data = vb.get_state()
    # Easter Egg: Wer ohne Spielstart hier reinstolpert, wird freundlich
    # auf die echte Repair-Café-Seite umgeleitet.
    if data["station"] < 1:
        return redirect("https://reparaturbahnhof.de/")
    vb.mark_task_done(1)
    return render_template("aufgabe_w3rkst4tt_k9p.html")


@app.route("/aufgabe_g0ldst3in_m7x")
def aufgabe_gold():
    vb.mark_task_done(2)
    return render_template("aufgabe_g0ldst3in_m7x.html")


@app.route("/aufgabe_l0r3nf4hrt_b3q")
def aufgabe_lore():
    vb.mark_task_done(3)
    return render_template("aufgabe_l0r3nf4hrt_b3q.html")


@app.route("/reset")
def reset():
    # Bestätigungs-Zwischenseite; der eigentliche Reset passiert erst
    # auf /reset/confirm, damit versehentliches Klicken oder Prefetching
    # nicht den Spielstand zerschießt.
    return render_template("reset.html")


@app.route("/reset/confirm")
def reset_confirm():
    before = vb.load_state()
    vb.save_state(vb._fresh())
    vb.log("INFO", "manual_reset",
           prev_station=before.get("station"),
           prev_task_done=before.get("task_done"))
    return render_template("reset_done.html")


@app.route("/logbook", methods=["GET"])
def logbook_view():
    return render_template("logbook_view.html",
                           entries=vb.read_logbook(),
                           just_submitted_ts=None,
                           finale_url="/aufgabe_st4rt_v9p",
                           format_duration=_format_duration)


@app.route("/logbook", methods=["POST"])
def logbook_submit():
    name    = (request.form.get("name")    or "").strip()[:100]
    comment = (request.form.get("comment") or "").strip()[:2000]
    if not name or not comment:
        vb.log("WARN", "logbook_rejected",
               reason="empty", has_name=bool(name), has_comment=bool(comment))
        return redirect("/aufgabe_st4rt_v9p")

    entry = vb.write_logbook_entry(name, comment)
    if entry is None:
        return redirect("/aufgabe_st4rt_v9p")

    return render_template("logbook_view.html",
                           entries=vb.read_logbook(),
                           just_submitted_ts=entry["timestamp"],
                           finale_url="/aufgabe_st4rt_v9p",
                           format_duration=_format_duration)


@app.route("/state")
def state():
    data = vb.get_state()
    return jsonify({
        "station":       data["station"],
        "task_done":     data["task_done"],
        "station_name":  vb.station_names.get(data["station"], "unknown"),
        "last_activity": data["last_activity"],
    })


# -------- Augsburg-Cache-Routen --------

@app.route("/augsburg/")
@app.route("/augsburg")
def augsburg_index():
    return redirect("/augsburg/start")


@app.route("/augsburg/start")
def augsburg_start():
    data, kind = aug.scan_start_qr()
    return render_template(
        "augsburg_start.html",
        kind=kind,
        station=data["station"],
        num_stations=aug.N,
    )


@app.route("/augsburg/station/<int:n>")
def augsburg_station(n):
    # n ist 1-basiert; intern 0-basiert.
    # TODO: Für den Einsatz echte QR-URLs mit obfuszierten Pfaden anlegen
    #       (analog zu /aufgabe_w3rkst4tt_k9p etc.).
    station_idx = n - 1
    if station_idx < 0 or station_idx >= aug.N:
        return redirect("/augsburg/start")
    aug.mark_task_done(station_idx)
    return render_template("augsburg_station.html",
                           station_num=n, num_stations=aug.N)


@app.route("/augsburg/state")
def augsburg_state():
    data = aug.get_state()
    return jsonify({
        "station":       data["station"],
        "task_done":     data["task_done"],
        "station_name":  aug.station_names.get(data["station"], "unknown"),
        "last_activity": data["last_activity"],
    })


@app.route("/augsburg/reset")
def augsburg_reset():
    return render_template("augsburg_reset.html")


@app.route("/augsburg/reset/confirm")
def augsburg_reset_confirm():
    before = aug.load_state()
    aug.save_state(aug._fresh())
    aug.log("INFO", "manual_reset",
            prev_station=before.get("station"),
            prev_task_done=before.get("task_done"))
    return render_template("reset_done.html")


@app.route("/augsburg/logbook", methods=["GET"])
def augsburg_logbook_view():
    return render_template("logbook_view.html",
                           entries=aug.read_logbook(),
                           just_submitted_ts=None,
                           finale_url="/augsburg/start",
                           format_duration=_format_duration)


@app.route("/augsburg/logbook", methods=["POST"])
def augsburg_logbook_submit():
    name    = (request.form.get("name")    or "").strip()[:100]
    comment = (request.form.get("comment") or "").strip()[:2000]
    if not name or not comment:
        aug.log("WARN", "logbook_rejected",
                reason="empty", has_name=bool(name), has_comment=bool(comment))
        return redirect("/augsburg/start")

    entry = aug.write_logbook_entry(name, comment)
    if entry is None:
        return redirect("/augsburg/start")

    return render_template("logbook_view.html",
                           entries=aug.read_logbook(),
                           just_submitted_ts=entry["timestamp"],
                           finale_url="/augsburg/start",
                           format_duration=_format_duration)


# -------- Server-Start --------

log_event("INFO", "server_start",
          state_dir=_data_dir, log_dir=LOG_DIR,
          vb_inactivity_min  = int(vb.inactivity_to.total_seconds() / 60),
          vb_arrived_min     = int(vb.arrived_to.total_seconds() / 60),
          aug_n              = aug.N,
          aug_inactivity_min = int(aug.inactivity_to.total_seconds() / 60),
          aug_arrived_min    = int(aug.arrived_to.total_seconds() / 60))


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
