# SBB Monitor

ESP32-S3 Abfahrtsmonitor für Schweizer Bahnhöfe (transport.opendata.ch).

Das Gerät wacht nach Zeitplan auf, holt die nächsten Abfahrten vom gewünschten Bahnhof und zeigt sie auf einem OLED-Display an. Ein NeoPixel signalisiert den schlechtesten Status der nächsten vier Züge. Danach kehrt es in den Deep Sleep zurück, um den Akku zu schonen.

## Hardware

| Komponente | Beschreibung |
|---|---|
| **Mikrokontroller** | ESP32-S3 (getestet auf ESP32-S3-DevKitC) |
| **Display** | SSD1306 128×64 OLED (I²C) |
| **Status-LED** | WS2812 NeoPixel |
| **Button** | Taster an GPIO 0 (Wake-up + Langdruck) |

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

Dort lassen sich einstellen:
- Bahnhof und Ziel-Filter
- Zeitfenster (bis zu 8, wann das Gerät aktiv sein soll)
- Wochenend-Schlaf-Fenster (Fr 18:00 → Mo 05:00, konfigurierbar)
- WiFi-Zugangsdaten (überschreibt secrets.h)
- LED-Farben, Verspätungs-Schwellen, Refresh-Intervalle
- GPIO-Belegung

Einstellungen werden in NVS gespeichert und überleben Neustarts.

### secrets.h (Fallback)

`WIFI_SSID` und `WIFI_PASS` in `main/secrets.h` werden verwendet, solange im Web-Panel keine eigenen Zugangsdaten gespeichert sind.

## Funktionsweise

### Aufwach- und Schlaf-Logik

1. RTC-Zeit prüfen — falls keine gültige Zeit vorhanden, NTP-Sync via WiFi.
2. Prüfen ob aktueller Zeitpunkt in einem aktiven Zeitfenster liegt und kein Wochenend-Schlaf aktiv ist.
3. **Außerhalb des Fensters:** Deep Sleep bis zum nächsten Fensterstart (max. `sleepMaxMin` Minuten). Im Wochenend-Fenster (`weekdaysOnly = true`) wird direkt bis zum Ende des Wochenend-Fensters geschlafen.
4. **Im Fenster oder per Button geweckt:** Aktiv-Schleife bis Fensterende.
5. Nach dem Fenster: Deep Sleep (Fallback 5 min).

### Aktiv-Schleife

Pro Iteration:
- Abfahrten von `transport.opendata.ch` abrufen (mit Retry).
- Bei Fehler: gecachte Daten anzeigen, solange sie `< staleMaxMin` Minuten alt sind.
- NeoPixel: schlechtester Status der nächsten 4 gültigen, nicht-ausgefallenen Züge.
  - Grün = pünktlich · Cyan = leicht verspätet · Lila = stark verspätet · Rot = Ausfall
- OLED: Abfahrtsliste mit Bahnhofname und Uhrzeit in der Kopfzeile.
- Adaptiver Refresh: je näher der nächste Zug, desto häufiger wird abgefragt.

### Ziel-Filter

Bis zu 4 Substring-Filter (case-insensitiv) auf Endstation und Zwischenhalte. Leer = alle Züge.

## Projektstruktur

```
main/
  main.c          — Hardware-Treiber und Hauptschleife
  sbb.c / sbb.h   — WiFi, HTTP, JSON-Parsing, Filter-Logik
  http_server.c   — Web-Panel (SPIFFS + /api/config)
  nvs_config.c    — Konfiguration in NVS lesen/schreiben
  cJSON.c         — Vendored JSON-Library
  spiffs/
    index.html    — Web-Panel UI (wird auf SPIFFS geflasht)
  secrets.h.example
```

## Build-System

Standard ESP-IDF v6 Projekt. Ziel: `esp32s3`.

Partition Table (`partitions.csv`):

| Name | Typ | Grösse |
|---|---|---|
| nvs | data/nvs | 24 KB |
| phy_init | data/phy | 4 KB |
| factory | app/factory | 1500 KB |
| storage | data/spiffs | 256 KB |
