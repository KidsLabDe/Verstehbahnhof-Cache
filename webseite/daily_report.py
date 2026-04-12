#!/usr/bin/env python3
"""Täglicher Telegram-Report für Verstehbahnhof-Cache + Augsburg Cache.

Parst das tägliche Log-File (LOG_DIR/YYYY-MM-DD.log) und die beiden
Gästebuch-Dateien, fasst die Aktivität zusammen und schickt eine
formatierte Nachricht an einen Telegram-Bot.

Umgebungsvariablen:
  TELEGRAM_BOT_TOKEN   Bot-Token von BotFather
  TELEGRAM_CHAT_ID     Ziel-Chat oder -Channel-ID
  LOG_DIR              Log-Verzeichnis (default: /app/log)
  REPORT_DATE          Datum für den Bericht (default: heute, YYYY-MM-DD)
"""

import json
import os
import re
import sys
import urllib.request
from datetime import datetime

LOG_DIR          = os.environ.get("LOG_DIR", "/app/log")
LOGBOOK_VB       = os.path.join(LOG_DIR, "logbook.jsonl")
LOGBOOK_AUG      = os.path.join(LOG_DIR, "logbook_augsburg.jsonl")
BOT_TOKEN        = os.environ.get("TELEGRAM_BOT_TOKEN", "")
CHAT_ID          = os.environ.get("TELEGRAM_CHAT_ID", "")

# Muss mit Definitionen in server.py übereinstimmen
VB_STATION_NAMES = {
    0: "Fürstenberg",
    1: "Berlin",
    2: "Leipzig",
    3: "Nürnberg",
    4: "Augsburg",
}


# -------- Telegram --------

def send_telegram(text: str) -> bool:
    if not BOT_TOKEN or not CHAT_ID:
        print("[report] Telegram nicht konfiguriert "
              "(TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID fehlen)",
              file=sys.stderr)
        return False
    url  = f"https://api.telegram.org/bot{BOT_TOKEN}/sendMessage"
    body = json.dumps({
        "chat_id": CHAT_ID,
        "text": text,
        "parse_mode": "HTML",
    }).encode()
    req = urllib.request.Request(
        url, data=body, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            resp.read()
        return True
    except Exception as exc:
        print(f"[report] Telegram-Fehler: {exc}", file=sys.stderr)
        return False


# -------- Log-Parsing --------

_KV = re.compile(r'(\w+)=("(?:[^"\\]|\\.)*"|-?\d+(?:\.\d+)?|true|false|null)')


def _parse_fields(raw: str) -> dict:
    result = {}
    for m in _KV.finditer(raw):
        k, v = m.group(1), m.group(2)
        try:
            result[k] = json.loads(v)
        except json.JSONDecodeError:
            result[k] = v
    return result


def parse_log(date_str: str) -> list:
    path = os.path.join(LOG_DIR, f"{date_str}.log")
    if not os.path.exists(path):
        return []
    events = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip()
            if not line:
                continue
            parts = line.split(" ", 3)
            if len(parts) < 3:
                continue
            ev = {
                "ts":    parts[0],
                "level": parts[1].strip(),
                "event": parts[2],
                "fields": _parse_fields(parts[3]) if len(parts) > 3 else {},
            }
            events.append(ev)
    return events


def read_logbook(path: str, date_str: str) -> list:
    if not os.path.exists(path):
        return []
    entries = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                e = json.loads(line)
                if (e.get("timestamp") or "").startswith(date_str):
                    entries.append(e)
            except json.JSONDecodeError:
                pass
    return entries


# -------- Formatierung --------

def _fmt_dur(sec) -> str:
    if sec is None:
        return "—"
    m, s = divmod(int(sec), 60)
    if m >= 60:
        h, m2 = divmod(m, 60)
        return f"{h}h {m2}m"
    return f"{m}m {s:02d}s"


def build_section(game_id: str, label: str,
                  events: list, lb: list,
                  station_names: dict | None = None) -> str:
    stats = {
        "started": 0, "advanced": 0, "pending": 0, "arrived": 0,
        "finished": 0, "resets": 0, "best_sec": None,
        "stations": {},
    }

    for ev in events:
        f = ev["fields"]
        if f.get("game") != game_id:
            continue
        name = ev["event"]
        if name == "start_qr_scan":
            kind = f.get("kind", "")
            if kind in stats:
                stats[kind] += 1
        elif name == "game_finished":
            stats["finished"] += 1
            dur = f.get("duration_sec")
            if isinstance(dur, (int, float)):
                if stats["best_sec"] is None or dur < stats["best_sec"]:
                    stats["best_sec"] = int(dur)
        elif name in ("inactivity_reset", "manual_reset"):
            stats["resets"] += 1
        elif name == "task_scan" and f.get("matched"):
            st = str(f.get("required", "?"))
            stats["stations"][st] = stats["stations"].get(st, 0) + 1

    lines = [f"\n<b>{label}</b>"]

    if stats["started"] == 0 and not lb:
        lines.append("  (kein Betrieb heute)")
        return "\n".join(lines)

    lines.append(
        f"  Spiele: {stats['started']}× gestartet"
        f"  |  {stats['finished']}× abgeschlossen"
    )
    if stats["resets"]:
        lines.append(f"  Resets: {stats['resets']}×")
    if stats["best_sec"] is not None:
        lines.append(f"  Rekord heute: {_fmt_dur(stats['best_sec'])}")

    if stats["stations"]:
        parts = []
        for k in sorted(stats["stations"], key=lambda x: int(x) if x.isdigit() else 0):
            sname = (station_names or {}).get(int(k), f"#{k}") if k.isdigit() else k
            parts.append(f"{sname}: {stats['stations'][k]}×")
        lines.append("  Stationen: " + "  ·  ".join(parts))

    if lb:
        lines.append(f"\n  📖 Gästebuch ({len(lb)} neu):")
        for e in lb[-5:]:
            ts      = (e.get("timestamp") or "")[-8:-3]
            name_e  = (e.get("name") or "?")[:25]
            comment = (e.get("comment") or "")
            short   = comment[:60] + ("…" if len(comment) > 60 else "")
            lines.append(f'  {ts}  {name_e}: „{short}"')

    return "\n".join(lines)


# -------- Hauptprogramm --------

def main():
    date_str = os.environ.get("REPORT_DATE",
                              datetime.now().strftime("%Y-%m-%d"))
    events   = parse_log(date_str)
    lb_vb    = read_logbook(LOGBOOK_VB,  date_str)
    lb_aug   = read_logbook(LOGBOOK_AUG, date_str)

    weekdays = ["Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"]
    try:
        dt     = datetime.strptime(date_str, "%Y-%m-%d")
        day    = weekdays[dt.weekday()]
        header = (f"<b>📊 Verstehbahnhof-Cache · "
                  f"{day} {dt.strftime('%d.%m.%Y')}</b>")
    except ValueError:
        header = f"<b>📊 Cache-Report {date_str}</b>"

    text  = header
    text += build_section("vb",  "🚂 Verstehbahnhof (Fürstenberg)",
                          events, lb_vb, VB_STATION_NAMES)
    text += build_section("aug", "🏰 Augsburg Cache",
                          events, lb_aug)

    print(text)
    ok = send_telegram(text)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
