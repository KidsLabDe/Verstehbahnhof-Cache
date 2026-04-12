#!/bin/bash
set -e

# Cron-Daemon bekommt normalerweise keine Container-Umgebungsvariablen.
# /etc/environment wird beim Cron-Start geladen und macht die Vars verfügbar.
printenv | grep -E '^(LOG_DIR|TELEGRAM_BOT_TOKEN|TELEGRAM_CHAT_ID|REPORT_DATE|STATE_DIR)=' \
    >> /etc/environment

# Cron-Daemon starten (im Hintergrund)
cron

# Gunicorn im Vordergrund – Logs landen auf stdout → docker compose logs
exec gunicorn \
    --bind 0.0.0.0:5000 \
    --workers 1 \
    --access-logfile - \
    server:app
