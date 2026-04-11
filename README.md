# Verstehbahnhof-Cache

Ein GeoCache für den [Verstehbahnhof](https://verstehbahnhof.de/) in Fürstenberg/Havel.

## Die Geschichte

Jim Knopf, Lukas der Lokomotivführer und die kleine Emma sind im Verstehbahnhof in Fürstenberg gestrandet — und sie wollen unbedingt wieder nach Hause nach **Augsburg zur Augsburger Puppenkiste**! Doch die Strecke ist lang, und Emma schafft das nicht in einem Rutsch. Sie muss an jedem Bahnhof auf der Route Halt machen, Wasser tanken und Feuer schüren.

Hier kommen die Cacher ins Spiel: An verschiedenen Stationen im und rund um den Verstehbahnhof sind **QR-Codes** versteckt. Wer einen QR-Code findet und scannt, hilft Emma, bis zum nächsten Bahnhof weiterzufahren. Auf einem **NeoPixel-Streifen**, der die Zugstrecke von Fürstenberg nach Augsburg darstellt, wandert Emma dann sichtbar bis zum nächsten Halt — Bahnhof für Bahnhof, QR-Code für QR-Code, bis Jim, Lukas und Emma endlich zu Hause bei der Puppenkiste ankommen.

## Wie es funktioniert

1. **Cacher findet QR-Code** an einer Station im Verstehbahnhof.
2. **QR-Code scannen** → ruft eine URL auf dem S1-Mini auf.
3. **S1-Mini** (ESP32-S1-Mini mit WLAN) empfängt den Aufruf.
4. **Emma fährt los**: Der NeoPixel-Streifen animiert die Lok von ihrem aktuellen Bahnhof zum nächsten.
5. **Am Bahnhof angekommen** bleibt Emma stehen und wartet auf den nächsten gefundenen QR-Code.
6. **Ziel erreicht**: Ist der letzte QR-Code gefunden, rollt Emma in Augsburg ein — große Ankunfts-Animation!

## Die Strecke

Der NeoPixel-Streifen bildet die Bahnstrecke Fürstenberg → Augsburg ab. Auf dem Weg liegen mehrere Bahnhöfe, die als feste Haltepunkte auf dem Streifen markiert sind. Jeder gefundene QR-Code befördert Emma genau einen Bahnhof weiter.

Geplante Stationen (Auswahl, wird noch finalisiert):

- Fürstenberg (Havel) — Start
- Berlin
- Leipzig
- Nürnberg
- Augsburg — Ziel (Augsburger Puppenkiste)

## Hardware

- **S1-Mini** (ESP32-S1-Mini) — WLAN-fähiger Mikrocontroller als Webserver und Animations-Steuerung
- **NeoPixel-LED-Streifen** (WS2812B) — visualisiert die Zugstrecke und die Bahnhöfe
- **QR-Codes** — auf Stationen im Verstehbahnhof verteilt, jeder Code triggert einen Zughalt weiter
- Stromversorgung, Gehäuse, Streckenkulisse (tbd)

## Software

- Firmware für den S1-Mini (Arduino/PlatformIO, tbd)
- Webserver mit Endpunkten pro QR-Code
- Animations-Engine für den NeoPixel-Streifen (Fahrt, Halt, Ankunft)
- Zustandsspeicher: an welchem Bahnhof steht Emma gerade?

## Status

Projekt-Initialisierung. Hardware-Konzept steht, Umsetzung folgt.

## Über den Verstehbahnhof

Der [Verstehbahnhof](https://verstehbahnhof.de/) in Fürstenberg/Havel ist ein Maker- und Lernort für Kinder und Jugendliche rund um Technik, Programmieren und Eisenbahn. Dieses Projekt ist ein kleiner GeoCache-Beitrag für Besucher:innen des Verstehbahnhofs.

## Lizenz

tbd
