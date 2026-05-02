# QR-Codes - Verstehbahnhof-Cache



---

<img src="/Users/kingbbq/src/Verstehbahnhof-Cache/qr-codes/01-start.png" alt="01-start" style="zoom:100%;" />

<img src="/Users/kingbbq/src/Verstehbahnhof-Cache/qr-codes/02-lok.png" alt="02-lok" style="zoom:50%;" />

<img src="/Users/kingbbq/src/Verstehbahnhof-Cache/qr-codes/03-repair-cafe.png" alt="03-repair-cafe" style="zoom:50%;" />

<img src="/Users/kingbbq/src/Verstehbahnhof-Cache/qr-codes/04-goldstein.png" alt="04-goldstein" style="zoom:50%;" />



<img src="/Users/kingbbq/src/Verstehbahnhof-Cache/qr-codes/05-lore.png" alt="05-lore" style="zoom:50%;" />



Druckbare QR-Codes für die fünf Stationen am Verstehbahnhof. Jeweils
10 Pixel pro Modul, 4 Module Rand, Error-Correction Level M. Hochauflösend
genug zum Drucken auf DIN-A6 oder größer.

Neu generieren:
```bash
qrencode -o <datei>.png -s 10 -m 4 -l M "<url>"
```

| Datei | Ziel-URL | Wo hinhängen |
|---|---|---|
| `01-start.png` | <https://verstehbahnhof.kidslab.de/aufgabe_st4rt_v9p> | **An Emma / am NeoPixel-Streifen.** Wird fünfmal gescannt (1× Start, 4× Bahnhof öffnen). Das ist _der_ zentrale QR-Code. |
| `02-lok.png` | <https://verstehbahnhof.kidslab.de/aufgabe1> | Am Lok-Bild irgendwo am Verstehbahnhof |
| `03-repair-cafe.png` | <https://verstehbahnhof.kidslab.de/aufgabe_w3rkst4tt_k9p> | Am Repair-Café-Plakat |
| `04-goldstein.png` | <https://verstehbahnhof.kidslab.de/aufgabe_g0ldst3in_m7x> | Am Goldstein auf dem Gelände |
| `05-lore.png` | <https://verstehbahnhof.kidslab.de/aufgabe_l0r3nf4hrt_b3q> | An der Lore / finale Cache-Box |
