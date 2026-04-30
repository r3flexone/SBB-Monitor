# Web-Interface Integration — Spezifikation für Claude Design

**Ziel:** Integriere das Web-Panel (`esp32/web/index.html`) in das bestehende ESP32-SBB-Monitor-Projekt, damit Benutzer alle Konfigurationen über den Browser ändern können, während das Gerät aktiv ist.

**Status:** Konzeptphase — diese Spec wird der Implementierung vorgelegt.

---

## 1. Übersicht der Integration

Das Web-Panel läuft auf dem ESP32 selbst:
- **HTTP-Server** (esp_http_server): Serviert `index.html` von SPIFFS
- **REST-API** (`/api/config`): GET zum Laden, POST zum Speichern
- **NVS-Persistierung** (Non-Volatile Storage): Config überlebt Neustarts
- **mDNS**: Erreichbar unter `http://blink.local`
- **Verfügbarkeit**: Nur während aktiver Zeitfenster oder nach Button-Druck

---

## 2. Hauptproblem: Config-Struktur ist unvollständig

Das aktuelle `main.c` hat ~26 `#define`-Konstanten; das Web-Panel (`nvs_config.h`) nur 17 Felder. **Lücken:**

### 2.1 Fehlende Felder in `blink_config_t`

```c
// AKTUELL (nvs_config.h):
typedef struct {
    int   startH, startM, endH, endM;        // 1 Zeitfenster
    int   buttonActiveS;                      // Knopfdauer
    int   refreshS;                           // Grundrefresh
    char  ssid[64], password[64];
    int   ntpTimeoutS;
    char  station[64];
    bool  sleepEnabled;
    int   sleepFallbackS, sleepAfterS, sleepMaxMin;
    int   ledGpio, sdaGpio, sclGpio;
    char  oledAddr[8];
    int   buttonGpio;
} blink_config_t;
```

**FEHLEN:**

| Konstante | Typ | Beschreibung | Default |
|-----------|-----|--------------|---------|
| `destFilters` | `char[4][32]` | Ziel-Filter (max 4) | leer |
| `destFilterCount` | `int` | Anz. Filter | 0 |
| `buttonLongPressMs` | `int` | Long-press Schwelle | 3000 |
| `buttonLongActiveMin` | `int` | Dauer nach long press | 10 |
| `weekdaysOnly` | `bool` | Nur Mo–Fr | 1 |
| `oledInvertMin` | `int` | Burn-in Invert-Intervall | 5 |
| `apiRetryCount` | `int` | API-Versuche | 3 |
| `apiRetryDelayMs` | `int` | Wartezeit zw. Versuchen | 5000 |
| `staleMaxMin` | `int` | Cache-Gültigkeit | 10 |
| `refreshNearSec` | `int` | <5min Refresh | 30 |
| `refreshMidSec` | `int` | 5-10min Refresh | 120 |
| `refreshFarSec` | `int` | 10-30min Refresh | 300 |
| `refreshVeryfarSec` | `int` | >30min Refresh | 600 |
| `refreshNearMin` | `int` | Schwelle für near | 5 |
| `refreshMidMin` | `int` | Schwelle für mid | 10 |
| `refreshFarMin` | `int` | Schwelle für far | 30 |
| `ledOkRgb` | `uint8_t[3]` | Grün (pünktlich) | {0,255,0} |
| `ledDelaySmallRgb` | `uint8_t[3]` | Cyan (leicht verspätet) | {0,255,255} |
| `ledDelayBigRgb` | `uint8_t[3]` | Lila (stark verspätet) | {128,0,255} |
| `ledCancelledRgb` | `uint8_t[3]` | Rot (Ausfall) | {255,0,0} |
| `ledLoadingRgb` | `uint8_t[3]` | Orange (laden) | {255,128,0} |
| `delaySmallMin` | `int` | Ab wann cyan (min) | 2 |
| `delayBigMin` | `int` | Ab wann lila (min) | 6 |
| `ledErrorBlinkMs` | `int` | Error-LED Blink-Periode | 500 |
| `timeWindows` | `struct[8]` | Multiple Zeitfenster | {6:45-6:55} |
| `timeWindowCount` | `int` | Anz. Zeitfenster | 1 |

**Total: 26 fehlende Config-Felder → nvs_config.h erweitern**

---

## 3. Architektur der Integration

### 3.1 Dateien die NEU erstellt/modifiziert werden

```
main/
  nvs_config.h        ← Erweitern (blink_config_t + 26 Felder)
  nvs_config.c        ← Erweitern (LOAD_INT/LOAD_STR für alle neuen Felder)
  http_server.h       ← Neu aus esp32/main/ kopieren
  http_server.c       ← Neu aus esp32/main/ kopieren
  spiffs/
    index.html        ← Neu aus esp32/web/ kopieren
  main.c              ← NICHT UMWANDELN, sondern:
    - #define bleibt für Compile-Time-Defaults
    - Nach NVS-Init optional via Config überschreiben (optional!)

CMakeLists.txt        ← Update: nvs_flash, spiffs, mdns, esp_http_server
sdkconfig.defaults    ← Update: PARTITION_TABLE_CUSTOM
partitions.csv        ← Neu (SPIFFS + NVS Partitionen)
secrets.h.example     ← Update: WiFi Creds weiterhin hardcoded oder auch von NVS?
```

### 3.2 Datenfluss

```
ESP32 startet
  ↓
app_main()
  ├─ nvs_flash_init()      ← vor http_server_start()
  ├─ http_server_start()   ← SPIFFS + API Handler registrieren
  ├─ mdns_init() + blink.local
  └─ sbb_wifi_init()       ← WiFi an
  
Benutzer öffnet http://blink.local
  ↓
Browser GET /
  ↓
send_file("/spiffs/index.html")  ← HTML + CSS + JS
  ↓
JavaScript: fetch('/api/config')  ← Laden aktueller Werte
  ↓
handler_config_get()
  ├─ nvs_config_load(&cfg)
  ├─ cJSON mit allen Feldern
  └─ POST-Body mit Passwort AUSGESCHLOSSEN
  
Benutzer ändert Werte + Speichern
  ↓
JavaScript: fetch('/api/config', {POST, JSON})
  ↓
handler_config_post()
  ├─ JSON parsen
  ├─ nvs_config_load(&cfg)  ← erst laden, dann überschreiben
  ├─ Neue Werte setzen
  ├─ nvs_config_save(&cfg)
  └─ {"ok":true} zurück
  
Gerät geht schlafen
  ↓
http_server_stop() vor go_to_sleep()  ← CPU/WiFi sparen
```

---

## 4. Power Consumption: HTTP-Server

**Antwort: Negligible — WiFi ist bereits an.**

Bei Längere Aktivität (Fenster 6:45–6:55):
- **WiFi Radio**: ~100 mA (ob Server läuft oder nicht — schon für API-Calls aktiv)
- **HTTP Server idle**: ~5 mA (FreeRTOS Task + I/O Multiplexing)
- **Summe Overhead**: ~5% wenn Server aktiv, vs. ~0% wenn nur Fetch
- **Ist akzeptabel**: Display (OLED) zieht ~10 mA, LED ~2 mA

**Deep Sleep**: Server wird vor `go_to_sleep()` gestoppt → alle Komponenten an.

**Fazit:** Kein messbarer Stromvorteil, Panel-Convenience überwiegt.

---

## 5. Schritt-für-Schritt Implementierung

### Phase 1: Struktur erweitern (nvs_config)

1. **nvs_config.h** — `blink_config_t` erweitern um alle 26 Felder
   - `timeWindows[8]` statt 1 Fenster
   - `destFilters[4][32]` + `destFilterCount`
   - Alle LED-Farben als `uint8_t[3]`
   - Alle Refresh-Schwellwerte

2. **nvs_config.c** — LOAD/SAVE macros erweitern
   - Für neue int-Felder: `LOAD_INT("apiRetryCount", apiRetryCount)` usw.
   - Für RGB-Arrays: `nvs_get_blob(h, "ledOkRgb", cfg->ledOkRgb, 3)` (oder als int32: R|G<<8|B<<16)
   - Für timeWindows: Loop über 8 und speichern als `"tw0_sh"`, `"tw0_sm"` usw.
   - Defaults: `nvs_config_defaults()` mit allen neuen Werten setzen

3. **Test**: `idf.py build` — keine Fehler, NVS-Prefix korrekt

### Phase 2: HTTP-Server + Spiffs einbinden

1. **http_server.h/.c** — Aus esp32/ kopieren
   - Handler erweitern für alle neuen config-Felder
   - `GET /api/config` → alle Felder (ausser Passwort)
   - `POST /api/config` → JSON parsen + alle neuen Felder

2. **spiffs/index.html** — Aus esp32/web/ kopieren
   - HTML-Panels für neue Settings (dest filters, LED farben, Thresholds, etc.)
   - JavaScript für loadConfig() / saveConfig() erweitern

3. **Partition Table**: `partitions.csv`
   ```
   # Name,   Type, SubType, Offset,   Size,    Flags
   nvs,      data, nvs,     0x9000,   0x6000,
   phy_init, data, phy,     0xf000,   0x1000,
   factory,  app,  factory, 0x10000,  1500K,
   storage,  data, spiffs,  ,         256K,
   ```

4. **CMakeLists.txt** (main/)
   ```cmake
   idf_component_register(
       SRCS
           "main.c" "sbb.c" "cJSON.c"
           "http_server.c" "nvs_config.c"    # NEU
       INCLUDE_DIRS "."
       REQUIRES driver esp_lcd esp_wifi esp_http_client
               esp_event nvs_flash
               esp_http_server spiffs mdns   # NEU
   )
   spiffs_create_partition_image(storage main/spiffs FLASH_IN_PROJECT)
   ```

5. **sdkconfig.defaults**
   ```
   CONFIG_PARTITION_TABLE_CUSTOM=y
   CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
   CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
   ```

### Phase 3: main.c Integration

**Philosophie:** #define bleibt für Code-Default, aber NVS kann überschreiben.

Option A: **Minimal** (empfohlen für MVP)
- main.c: `#define` unverändert
- Nach `nvs_config_load(&cfg)`: optional `STATION = cfg.station`, `STATION = cfg.ssid` etc. (nur Strings, nicht ganze Arrays)
- Vorteil: Altlasten bleiben compilierbar, Panel nur für Laufzeit-Tweaks

Option B: **Vollständig** (später)
- Alles aus `cfg` laden
- #define nur als Defaults in der NVS-Init
- Mehr Arbeit, aber konsistenter

**Empfehlung:** Starten mit Option A. http_server wird nur aufgerufen wenn Gerät wach ist, dann sind NVS-Werte verfügbar.

### Phase 4: Init-Reihenfolge in app_main()

```c
void app_main(void) {
    uint32_t wakeup = esp_sleep_get_wakeup_causes();
    
    // 1. NVS GANZ OBEN (vor sbb_wifi_init!)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    
    // 2. Config laden
    blink_config_t cfg;
    nvs_config_load(&cfg);
    
    // 3. LED/OLED init
    led_init();
    oled_init_display();
    
    // ... rest der Logik (wie jetzt) ...
    
    // 4. Nach WiFi-Verbindung: HTTP-Server + mDNS starten
    esp_netif_init();
    mdns_init();
    mdns_hostname_set("blink");
    mdns_instance_name_set("Blink ESP32");
    http_server_start();  // ← NUR wenn wach
    
    // ... Hauptschleife ...
    
    // 5. Vor go_to_sleep(): Server stoppen
    http_server_stop();
    go_to_sleep(5 * 60 * 1000000);
}
```

### Phase 5: QA Checklist

- [ ] `idf.py fullclean && idf.py build` erfolgreich
- [ ] Flash: `idf.py flash monitor`
- [ ] Browser: `http://blink.local` lädt index.html
- [ ] Panel: GET `/api/config` gibt alle aktuellen Werte
- [ ] Panel: POST `/api/config` mit neuen Werten speichert in NVS
- [ ] Nach Neustart: Alte Werte sind noch da
- [ ] LED-Farben: Alle RGB-Werte sichtbar auf dem Status-Tab
- [ ] Zeitfenster: Min. 1, max. 8 Fenster definierbar
- [ ] Filter: Dest-Filter in NVS persistiert, API nutzt sie
- [ ] OLED: Invert-Intervall funktioniert wie im Code
- [ ] Fehlerfall: POST mit ungültigem JSON → Error 400

---

## 6. HTML-Panel: Neue Sections

Das aktuelle Panel hat bereits:
- **Zeitfenster** (aber nur 1)
- **Netzwerk** (SSID, Passwort, NTP, Station)
- **Schlaf** (Deep-Sleep)
- **Hardware** (GPIO, I²C, OLED)
- **Status** (LED, Clock, WiFi, OLED-Vorschau)

**Zu erweitern:**

1. **Zeitfenster → Multiple Windows**
   - Button "Fenster hinzufügen" (max 8)
   - Für jedes: Start/End mit Validierung (Start < End)
   - Validierung: Overlap-Warnung

2. **Neue Section: Ziel-Filter**
   - Text-Input × 4
   - Hinweis: "Leer = alle Züge"
   - Substring-Match, case-insensitive

3. **Neue Section: LED-Konfiguration**
   - Color Picker × 5 (OK, DelaySmall, DelayBig, Cancelled, Loading)
   - oder RGB Slider (R 0-255, G 0-255, B 0-255)
   - Status-Live-Vorschau

4. **Neue Section: API & Refresh**
   - API Retry Count, Retry Delay (ms)
   - Stale-Data Max (min)
   - Refresh Thresholds: near/mid/far/veryfar (sec)
   - Refresh Tiers: near/mid/far (min)

5. **Neue Section: Verhalten**
   - Weekdays Only (toggle)
   - OLED Invert (min, oder 0=aus)
   - LED Error Blink (ms)
   - Button Long Press (ms)
   - Button Long Active (min)
   - Delay Thresholds: Small (min), Big (min)

---

## 7. Fehler-Fälle & Fallbacks

| Fehler | Behandlung |
|--------|-----------|
| SPIFFS mount fehlgeschlagen | HTTP Server startet nicht, Gerät funktioniert normal ohne Panel |
| NVS voll | `nvs_flash_erase()` beim Boot, alle Defaults neu setzen |
| POST ungültiges JSON | HTTP 400, Config ändert sich nicht |
| POST RGB außerhalb 0-255 | Clipping im Handler oder UI-Validierung |
| Timewindow Overlap | Warnung im Panel, aber speichern erlaubt (Reihenfolge matters) |
| WiFi disconnect während POST | Anfrage abbricht, Config bleibt unverändert |

---

## 8. Zukünftige Ideen (nicht in MVP)

- **OTA Web Updater** — Neue Firmware via Panel flashen
- **Syslog Streaming** — Logs im Panel live sehen
- **Statistik** — Anzahl Abfahrten pro Tag, Zug-Pünktlichkeit
- **Mehrsprachigkeit** — DE/EN/FR Toggle im Panel
- **Dark/Light Mode** — Panel hat bereits Dark (gut!)
- **Mobile Responsive** — Ipad-optimiert (derzeit nur Desktop)

---

## 9. Abhängigkeiten & Versionen

- **ESP-IDF**: v6.0 (minimal build)
- **Components**: driver, esp_http_server, nvs_flash, spiffs, mdns, esp_wifi
- **cJSON**: bereits vorhanden (vendored)
- **Fonts**: Existing 5×7 Font + Umlaute, reichen für Panel auch

---

## 10. Glossar

| Begriff | Erklärung |
|---------|-----------|
| NVS | Non-Volatile Storage — ESP32 Persistierungs-API |
| SPIFFS | SPI Flash File System — Dateiystem für kleine Flash-Partitionen |
| mDNS | Multicast DNS — `.local` Namen ohne DNS-Server |
| REST API | HTTP GET/POST für Daten-Austausch |
| Deep Sleep | Energiesparmodus mit minimaler Stromaufnahme |
| Worst-of-4 Rule | LED zeigt schlimmsten Status aller 4 Züge |

---

## Summary für Claude Design

**Die Aufgabe:**
1. Erweitere `blink_config_t` um 26 fehlende Felder
2. Passe NVS load/save macros an
3. Kopiere http_server.c/h aus esp32/
4. Integriere spiffs/index.html mit erweiterten Panels
5. Update CMakeLists.txt + sdkconfig.defaults + partitions.csv
6. Koordiniere main.c Init-Reihenfolge (nvs zuerst, server nach wifi)
7. Test: Spiel alle Panels durch, speichern funktioniert, Neustarts erhalten Werte

**Schwierigkeitsgrad:** 3/10 — meiste Code existiert schon, Copy-Paste + Config-Felder Add.

**Zeiteaufwand:** ~2-3 Stunden mit Testing.

---

**Erstellt:** 2026-04-30  
**Für:** Claude Design Projekt  
**Status:** Freigegeben zur Implementierung
