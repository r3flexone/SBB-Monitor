# SBB Monitor

ESP32-S3 Abfahrtsmonitor für Schweizer Bahnhöfe (transport.opendata.ch).

Das Gerät wacht nach Zeitplan auf, holt die nächsten Abfahrten vom gewünschten Bahnhof und zeigt sie auf einem OLED-Display an. Ein NeoPixel signalisiert den schlechtesten Status der nächsten vier Züge. Danach kehrt es in den Deep Sleep zurück, um den Akku zu schonen.

## Hardware

| Komponente | Beschreibung |
|---|---|
| **Mikrokontroller** | ESP32-S3 (getestet auf ESP32-S3-DevKitC) |
| **Display** | SSD1306 128×64 OLED (I²C) |
| **Status-LED** | WS2812 NeoPixel |
| **Button** | Taster an GPIO 0 (Wake-up + Langdruck + Sleep-Taste) |

### Verdrahtung (Standardwerte)

| Signal | GPIO |
|---|---|
| NeoPixel DATA | 48 |
| OLED SDA | 4 |
| OLED SCL | 5 |
| OLED I²C Adresse | 0x3C |
| Button | 0 |

Alle Pins sind über das Web-Panel oder NVS konfigurierbar.

## Einrichtung

```bash
# 1. WiFi-Zugangsdaten eintragen (wird von Git ignoriert)
cp main/secrets.h.example main/secrets.h
# WIFI_SSID und WIFI_PASS in secrets.h anpassen

# 2. Ziel-Chip setzen
idf.py set-target esp32s3

# 3. Bauen und flashen
idf.py build flash monitor   # Ctrl-] zum Beenden
```

> Beim ersten Mal oder nach Änderung der Partition Table:
> `idf.py fullclean` vor dem Build ausführen.

## Konfiguration

### Web-Panel (empfohlen)

Wenn das Gerät aktiv ist (im Zeitfenster oder per Button geweckt), ist das Konfigurations-Panel unter **http://sbb-monitor.local** erreichbar.

Das Panel zeigt oben rechts an ob der ESP gerade **Online** oder **schläft** — Einstellungen können nur bei Online-Status gespeichert werden.

Dort lassen sich einstellen:

- **Zeitfenster** — bis zu 8 aktive Zeitfenster (wann das Gerät aktiv sein soll)
- **Tastendruck** — Aktiv-Dauer nach Kurz- und Langdruck
- **Bahnhof & Ziel-Filter** — Station und bis zu 4 Substring-Filter
- **Schlaf** — Deep-Sleep ein/aus, Fallback-Dauer, Max-Schlafdauer, Schlaf nach Fenster
- **Wochenend-Schlaf** — eigener, unabhängig schaltbarer Schlaf-Zeitraum (z. B. Fr 18:00 → Mo 05:00)
- **Nur Wochentage** — Sa/So kein normales Zeitfenster aktiv
- **LED-Farben** — RGB-Farben für alle Zustände (pünktlich, verspätet, Ausfall, Laden)
- **Verspätungs-Schwellen** — ab wann Cyan bzw. Lila
- **API & Refresh-Intervalle** — adaptiver Refresh, Retry, Cache-Gültigkeit
- **Hardware** — GPIO-Belegung für LED, OLED, Button
- **OLED Invert** — periodisches Invertieren gegen Einbrennen

Einstellungen werden in NVS gespeichert und überleben Neustarts und Deep Sleep.

### secrets.h (Fallback)

`WIFI_SSID` und `WIFI_PASS` in `main/secrets.h` werden verwendet, solange im Web-Panel keine eigenen Zugangsdaten gespeichert sind.

## Funktionsweise

### Aufwach- und Schlaf-Logik

1. RTC-Zeit prüfen — falls keine gültige Zeit vorhanden, NTP-Sync via WiFi.
2. Prüfen ob aktueller Zeitpunkt in einem aktiven Zeitfenster liegt.
3. **Außerhalb des Fensters und `sleepEnabled = true`:** Deep Sleep bis zum nächsten Fensterstart (max. `sleepMaxMin` Minuten). Im Wochenend-Schlaf-Fenster (`weekendSleepEnabled = true`) wird direkt bis zum Ende des Wochenend-Fensters geschlafen.
4. **`sleepEnabled = false`:** Gerät bleibt dauerhaft aktiv, der Fortschrittsbalken auf dem OLED bleibt voll. Sobald Sleep über das Web-Panel wieder aktiviert wird, startet ein frischer Timer (`buttonActiveMin` Minuten) ab dem Speicherzeitpunkt.
5. **Im Fenster oder per Button geweckt:** Aktiv-Schleife bis Fensterende.
6. **Button während aktivem Betrieb:** Sofortiger Deep Sleep.
7. Nach dem Fenster: Deep Sleep für `sleepAfterS` Sekunden (Standard: 300 s = 5 min).

### Button-Verhalten

| Aktion | Effekt |
|---|---|
| Kurzdruck (Wakeup) | `buttonActiveMin` Minuten aktiv (Standard: 10 min) |
| Langdruck (Wakeup) | `buttonLongActiveMin` Minuten aktiv |
| Druck während Betrieb | Sofort in Deep Sleep |

### Aktiv-Schleife

Pro Iteration:
- Abfahrten von `transport.opendata.ch` abrufen (mit Retry).
- Bei Fehler: gecachte Daten anzeigen, solange sie `< staleMaxMin` Minuten alt sind.
- NeoPixel: schlechtester Status der nächsten 4 gültigen, nicht-ausgefallenen Züge.
  - Grün = pünktlich · Cyan = leicht verspätet · Lila = stark verspätet · Rot = Ausfall
- OLED: Abfahrtsliste mit Bahnhofname und Uhrzeit in der Kopfzeile.
- Fortschrittsbalken unten: verbleibende Aktiv-Zeit (voll wenn Sleep deaktiviert).
- Adaptiver Refresh: je näher der nächste Zug, desto häufiger wird abgefragt.

### Ziel-Filter

Bis zu 4 Substring-Filter (case-insensitiv) auf Endstation und Zwischenhalte. Leer = alle Züge.

## Projektstruktur

```
main/
  main.c          — Hardware-Treiber und Hauptschleife
  sbb.c / sbb.h   — WiFi, HTTP, JSON-Parsing, Filter-Logik
  http_server.c   — Web-Panel (SPIFFS + /api/config + /api/status)
  nvs_config.c    — Konfiguration in NVS lesen/schreiben
  cJSON.c         — Vendored JSON-Library
  spiffs/
    index.html    — Web-Panel UI (wird auf SPIFFS geflasht)
  secrets.h.example
```

## Build-System

Standard ESP-IDF v5.x Projekt. Ziel: `esp32s3`.

Partition Table (`partitions.csv`):

| Name | Typ | Grösse |
|---|---|---|
| nvs | data/nvs | 24 KB |
| phy_init | data/phy | 4 KB |
| factory | app/factory | 1500 KB |
| storage | data/spiffs | 256 KB |
