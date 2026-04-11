import json
import os
from datetime import datetime, timedelta
from flask import Flask, render_template, jsonify, redirect

app = Flask(__name__)

_data_dir = os.environ.get("STATE_DIR", os.path.dirname(__file__))
os.makedirs(_data_dir, exist_ok=True)
STATE_FILE = os.path.join(_data_dir, "state.json")
INACTIVITY_TIMEOUT = timedelta(minutes=30)

STATION_NAMES = {
    0: "start",
    1: "lock",
    2: "repair_cafe",
    3: "goldstein",
    4: "lore",
}


def load_state():
    if not os.path.exists(STATE_FILE):
        return {"state": 0, "last_activity": datetime.now().isoformat()}
    with open(STATE_FILE) as f:
        return json.load(f)


def save_state(state_dict):
    with open(STATE_FILE, "w") as f:
        json.dump(state_dict, f)


def get_state():
    data = load_state()
    last = datetime.fromisoformat(data["last_activity"])
    if datetime.now() - last > INACTIVITY_TIMEOUT:
        data = {"state": 0, "last_activity": datetime.now().isoformat()}
        save_state(data)
    return data


def advance_state(required_state, next_state):
    data = get_state()
    if data["state"] == required_state:
        data["state"] = next_state
        data["last_activity"] = datetime.now().isoformat()
        save_state(data)
    elif data["state"] >= required_state:
        data["last_activity"] = datetime.now().isoformat()
        save_state(data)
    return data


def touch_state():
    data = get_state()
    data["last_activity"] = datetime.now().isoformat()
    save_state(data)
    return data


@app.route("/")
def index():
    touch_state()
    return render_template("index.html")


@app.route("/aufgabe1")
def aufgabe1():
    advance_state(required_state=0, next_state=1)
    return render_template("aufgabe1.html")


@app.route("/aufgabe2")
def aufgabe2():
    touch_state()
    return render_template("aufgabe2.html")


@app.route("/aufgabe_w3rkst4tt_k9p")
def aufgabe_repair():
    data = get_state()
    if data["state"] < 1:
        return redirect("https://reparaturbahnhof.de/")
    advance_state(required_state=1, next_state=2)
    return render_template("aufgabe_w3rkst4tt_k9p.html")


@app.route("/aufgabe_g0ldst3in_m7x")
def aufgabe_gold():
    advance_state(required_state=2, next_state=3)
    return render_template("aufgabe_g0ldst3in_m7x.html")


@app.route("/aufgabe_l0r3nf4hrt_b3q")
def aufgabe_lore():
    advance_state(required_state=3, next_state=4)
    return render_template("aufgabe_l0r3nf4hrt_b3q.html")


@app.route("/state")
def state():
    data = get_state()
    return jsonify({
        "state": data["state"],
        "station": STATION_NAMES.get(data["state"], "unknown"),
        "last_activity": data["last_activity"],
    })


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
