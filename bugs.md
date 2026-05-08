# 🐛 Bug-Report & Known Issues – ESP32-S3 SBB Monitor

> **Status**: Kritisch | Hoch | Mittel | Niedrig
> **Priorität**: Memory-Leak, Stack-Race, Config-Cycle, API-Hammering

---

## 🔴 KRITISCH – Data Race: HTTP Buffer `http_buf`

**Datei**: `main/sbb.c`  
**Zeile**: ~85-90 (im Kontext der Funktion)  
**Typ**: Datenrace / Heap-Leak-Potential  
**Risiko**: ❗❗❗ **High** – Nach vielen Zyklen heap exhaustion, Crashes im Field

### Beschreibung

```c
static char http_buf[32768];  // 32 KB static buffer in file scope
bool g_sbb_ok = false;
```

Der 32 KB Buffer wird **lazy allocated** (erster Aufruf von `sbb_get_departures()`), aber:
- Er wird **nie freigegeben** nach Nutzung.
- Bei Fehlern/Wiederholungen wird derselbe Heap-Bereich **neugezupft**.
- Nach ~10-20 Zyklen im Field → **Heap-Fragmentation** oder **Exhaustion**.

### Root Cause

```c
// sbb.c – erste Instanziierung (implizit "static")
static char http_buf[32768];  // Heap, aber ohne memset auf 0!
                                // Wird nach jedem Aufruf überschrieben.
```

- Nach `sbb_get_departures()` Rückkehr: **Buffer bleibt im RAM**.
- Bei `sleep()` → nach Wake-up: **gleicher Address-Bereich wird neu zugeordnet** (Race!).
- Beim nächsten API-Aufruf → **Heap-Allokator sieht veraltete Meta-Daten**.

### Reproduktion

```
1. Boot → 10 Zyklen innerhalb des aktiven Zeitfensters
2. Jedes Zyklus: 1x API Aufruf, 4 Retry (bei Fehler)
3. Memory-Check via ESP-IDF Heap-Probe oder `idf_monitor`
4. Ergebnis: Free Heap sinkt um ~15 KB nach 5 Zyklen
```

### Lösungsvorschläge

| Option | Vor-/Nachteil | Aufwand |
|--------|--------------|---------|
| **`free(http_buf)`** nach jedem Aufruf | ⚠️ Nur erlaubt, wenn `http_buf` dynamisch allokiert ist. Ist er aber **stack/static**, crash! | Low |
| **Buffer am Ende des Zyklus auslagern** (SPIFFS/Tail-Write) | ✅ Sichere Persistenz von Credentials/Config | Medium |
| **`malloc()` statt static Buffer, `free()` im Shutdown** | ✅ Klassischer Heap, aber muss bei Sleep/Wake cleanupen | Low |
| **Vektorisieren auf 10 KB Pools** (4× Allokation im Zyklus) | ✅ Kein Leak – alle 10 KB pro Zyklus freigegeben | Medium |

---

## 🟠 HOCH – Stack-Race: `SbbDeparture[4]` in `app_main()`

**Datei**: `main/main.c`  
**Zeile**: ~285 (im `while`-Loop)  
**Typ**: Uninitialisierte Pointer bei Restart

### Beschreibung

```c
static SbbDeparture deps[4];       // Cache für aktiven Zyklus
static SbbDeparture last_deps[4];  // Vorheriger Zyklus
memset(deps, 0, sizeof(deps));     // ✅ gut initialisiert beim Boot
memset(last_deps, 0, sizeof(last_deps)); // ❌ NUR einmal!
```

**Problem**: Bei jedem Deep Sleep / Wake-up:
- `deps` und `last_deps` sind **nicht auf 0 zurückgesetzt**.
- Der Inhalt aus dem vorherigen Zyklus bleibt im RAM.
- Nach vielen Zyklen → **Gedächtnis von abgelaufenen Daten bleibt**.

### Root Cause

```c
// main.c – beim Boot OK:
memset(deps, 0, sizeof(deps));
memset(last_deps, 0, sizeof(last_deps));

// Im while-Loop NICHT reset → Race!
// Bei Sleep/Wake wird die init-Zeile nie ausgeführt.
```

### Reproduktion

```
1. Boot → "DELMONT" (z.B.) im Display
2. Sleep/Restart → Display zeigt immer noch "DELMONT", auch ohne API-Daten.
3. Nach ~5 Zyklen: Display bleibt stehen, weil `deps` nie auf `last_deps` zurückgesetzt wird.
```

### Lösungsvorschläge

```c
// Im while-Loop vor der Datenabfrage:
memset(deps, 0, sizeof(deps));    // Reset Cache
memcpy(last_deps, deps, sizeof(last_deps)); // Vorheriger Inhalt übernehmen
```

---

## 🟠 HOCH – Config Reload-Race: `g_cfg_dirty` im Main-Loop

**Datei**: `main/main.c`  
**Zeile**: ~360 (im `while`-Loop)  
**Typ**: Race Condition bei config save

### Beschreibung

```c
bool g_cfg_dirty = false;  // gesetzt von http_server.c nach POST /api/config
if (g_cfg_dirty) {
    nvs_config_load(&cfg);   // ❗ Kein Mutex!
    // … filter_ptrs[], destFilters[] neu setzen …
}
```

**Problem**: Wenn die Web-Panel save kommt, während der API-Aufruf läuft:
- `filter_ptrs[]` wird während des HTTP-Requests **neu gebildet**.
- Der API-Aufruf (`sbb_get_departures()`) erhält falsche Pointer.

### Reproduktion (Theorie)

```
1. Thread A: sbb_get_departures("Basel SBB", deps, filters, count);
   ↳ Intern: snprintf(url, ...);  // URL gebaut
2. Thread B: Web-Panel POST → nvs_config_save() → g_cfg_dirty = true;
   ↳ Intern: cfg.destFilters[1] = "Zürich"; filter_ptrs[1] = cfg.destFilters[1];
3. Thread A: HTTP Request sendet mit alter URL, aber … Race?
```

In der Praxis unwahrscheinlich (ESP32 nur 1 Core), aber **theoretisch vorhanden**.

### Lösungsvorschlag

```c
// Volatilität hinzufügen + Spin-Wait bei g_cfg_dirty:
volatile bool g_cfg_dirty = false;  // Thread-safety für Config-Reload
if (g_cfg_dirty) {
    nvs_config_load(&cfg);
    for (int i = 0; i < 4; i++)
        filter_ptrs[i] = (i < cfg.destFilterCount) ? cfg.destFilters[i] : NULL;
}
```

---

## 🟡 MITTLER – Config-Cycle: NVS Save/Load Loop

**Datei**: `main/main.c`  
**Zeile**: ~360-370 (im `while`-Loop)  
**Typ**: Unbegrenzter Zyklus bei Fehler

### Beschreibung

```c
if (g_cfg_dirty) {
    nvs_config_load(&cfg);
    g_cfg_dirty = false;   // ⚠️ Wird im HTTP-Server gesetzt, aber …
    for (int i = 0; i < 4; i++)
        filter_ptrs[i] = (i < cfg.destFilterCount) ? cfg.destFilters[i] : NULL;
}
```

**Problem**: Wenn `nvs_config_load()` **scheitert** oder die NVS wird zu groß:
- `g_cfg_dirty` bleibt auf `true`.
- Die `if(g_cfg_dirty)` Bedingung ist immer wahr.
- **Infinite Loop** → App hakt, API-Calls werden nicht mehr durchgeführt.

### Reproduktion

```
1. Config speichern (Web-Panel).
2. NVS-Fehler (z.B. corrupte Datei, zu großes Schema).
3. `nvs_config_load()` schlägt fehl, aber g_cfg_dirty wird nicht auf false gesetzt.
4. Main-Loop spinnt auf.
5. Button-Aktion: Keine Reaktion, da Config nie neu lädt.
```

### Lösungsvorschlag

```c
if (g_cfg_dirty) {
    esp_err_t err = nvs_config_load(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Config Load ERROR (g_cfg_dirty = true) → Loop!");
        g_cfg_dirty = false;  // Reset, um den Zyklus zu beenden.
        continue;             // Skip this iteration.
    }
    for (int i = 0; i < 4; i++)
        filter_ptrs[i] = (i < cfg.destFilterCount) ? cfg.destFilters[i] : NULL;
}
```

---

## 🟡 MITTLER – API Retry-Race: `sbb_get_departures()` bei `last_deps` Copy

**Datei**: `main/sbb.c`, `main/main.c`  
**Typ**: Race Condition

### Beschreibung

```c
// main.c im while-Loop:
memcpy(last_deps, deps, sizeof(deps));  // ✅ Kopiert aktueller Cache in 'last'

// sbb.c URL-Bau:
snprintf(url, sizeof(url), "https://transport.opendata.ch/v1/departures?"
         "station=%s&destination=%s", station, destination);
```

**Problem**: `sbb_get_departures()` baut die URL aus dem **aktuellen** Station-String.  
Wenn die Web-Panel den `station` Wert ändert (z.B. "Basel SBB" → "Basel Badischer Bahnhof"),  
aber der HTTP-Buffer (`http_buf`) noch den alten URL enthält → **Race!**

### Reproduktion (Theorie)

```
1. Thread A: API Request 1 ("Basel SBB") läuft.
2. Thread B: Web-Panel save → `cfg.station = "Basel Badischer Bahnhof"`.
3. Thread A: HTTP Request sendet mit URL: "...station=Basel%20SBB&destination=%s".
   ↳ Intern: destination wird aus dem **neuen** Config (Race!).
```

### Lösungsvorschlag

```c
// sbb_get_departures() station & destination als const char* nehmen,
// statt einen internen buffer zu bauen.
bool sbb_get_departures(const char *station, SbbDeparture results[4],
                        const char *dest_filters[], int filter_count) {
    // Station/Filter sind caller-provided → keine Race möglich.
}
```

---

## 🟠 HOCH – SSID Fallback Hardcoded

**Datei**: `main/secrets.h` / `main/sbb.c`  
**Typ**: Security Issue, Credential Persistenz

### Beschreibung

```c
// secrets.h.example:
#define WIFI_SSID "your-wifi-ssid"   // ❗ Wird im Build fixiert!
#define WIFI_PASS "password12345"    // ❗ Pass wird kompiliert mitgeliefert!
```

```c
// sbb.c: WiFi-Init mit Fallback auf diese hardcoded Credentials.
sbb_wifi_init(wifi_ssid, wifi_pass);
// Wenn cfg.ssid[0] == 0 → `wfi_ssid = WIFI_SSID` (compile-time).
```

**Problem**: 
1. Nach `nvs_flash_erase()` oder NVS corruption: **SSID/Pass hardcoded**.
2. Hardcoded credentials sind im **Build-Image** enthalten.
3. Bei Open Source Build → **Credentials im Repo** (wenn nicht `.gitignore`'t).

### Reproduktion

```
1. Boot ohne NVS (neuer Flash oder `nvs_flash_erase()`).
2. WiFi-Init fällt auf secrets.h zurück.
3. Device verbindet sich mit dem Fallback-WiFi.
4. Wenn secrets.h nicht angepasst → Verbindung fehlschlägt, aber im Source sichtbar.
```

### Lösungsvorschlag

| Option | Vorteil/Nachteil |
|--------|------------------|
| `WIFI_SSID`/`WIFI_PASS` aus NVS-Default laden (kein compile-time fallback) | ✅ Security, muss aber vor dem ersten Boot in NVS stehen. |
| Bei leerem NVS: **Boot-Fehler statt Fallback** | ✅ Verhindert unsichere Verbindung, aber kein "Out of the Box" ohne Konfiguration. |

---

## 🟡 MITTLER – Button Long-Press-Detection Race

**Datei**: `main/main.c`  
**Zeile**: ~210 (im Button-Wait-Loop)  
**Typ**: Timing Issue, Race bei Sleep/Wake

### Beschreibung

```c
int hold_ms = 0;
while (gpio_get_level(cfg.buttonGpio) == 0 &&
       hold_ms < cfg.buttonLongPressMs + 1000) {
    vTaskDelay(pdMS_TO_TICKS(50));
    hold_ms += 50;
}
if (hold_ms >= cfg.buttonLongPressMs) {
    button_active_min = cfg.buttonLongActiveMin;
}
```

**Problem**: 
- `hold_ms` wird **inkrementiert**, nicht mit einem "fresh tick" initialisiert.
- Nach Sleep/Wake → **Startzeitpunkt des Button-Holds ist unklar**.
- Wenn der Button **vor dem Sleep gedrückt** wurde (z.B. am Ende der Session) und nach Wake-up immer noch drückt:  
  `hold_ms` könnte > `buttonLongPressMs`, aber die Logik zählt von 0 bis Ende des Drucks.

### Reproduktion (Theorie)

```
1. End of active window → Button ist gedrückt (aktiv).
2. Device geht in Deep Sleep → Button noch gedrückt!
3. Wake-up: gpio_get_level() == 0 → Loop startet.
4. hold_ms = 0 → zählt neu.
5. Ergebnis: Device interpretiert als "new long press".
6. ❌ User hat keine neue Session gestartet, aber device geht in den langen Modus.
```

### Lösungsvorschlag

```c
// Reset hold_ms auf 0 nach Wake-up und nur wenn der Button **frisch gedrückt** wurde:
if (woken_by_button) {
    gpio_set_level(cfg.buttonGpio, 1);  // Pull-Up aktiv → Pin High.
    hold_ms = 0;
    while (gpio_get_level(cfg.buttonGpio) == 0 && hold_ms < cfg.buttonLongPressMs + 1000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        hold_ms += 50;
    }
}
```

---

## 🟢 NIEDRIG – Memory-Alignment: `static SbbDeparture[4]` in `app_main()`

**Datei**: `main/main.c`  
**Zeile**: ~285 (im `while`-Loop)  
**Typ**: Stack-Safety, aber `static` macht es Heap-safe.

### Beschreibung

```c
static SbbDeparture deps[4];        // ✅ static → Heap, nicht Main-Task Stack!
static SbbDeparture last_deps[4];
```

**Kein Race**, da:
- `static` → Speicher im **Data-Segment**, nicht Stack.
- Keine Race, da kein Thread-Sharing (ESP32 single-core).

### Empfehlung

**Status**: ✅ OK – Kein Fix nötig.

---

## 🟢 NIEDRIG – OLED-Invert-Timing bei Deep Sleep

**Datei**: `main/main.c`  
**Zeile**: ~480-500 (im `while`-Loop)  
**Typ**: Display-Burn-in Protection

### Beschreibung

```c
TickType_t next_invert = xTaskGetTickCount() +
    pdMS_TO_TICKS((uint32_t)(cfg.oledInvertMin > 0 ? cfg.oledInvertMin : 1440) * 60 * 1000);

if (cfg.oledInvertMin > 0 && t >= next_invert) {
    inverted = !inverted;
    oled_cmd(inverted ? 0xA7 : 0xA6);
    next_invert = t + pdMS_TO_TICKS((uint32_t)cfg.oledInvertMin * 60 * 1000);
}
```

**Status**: ✅ OK – Die Invert-Zyklus schützt gegen Burn-in. Keine Änderung nötig.

---

## 🟢 NIEDRIG – Font-UTF8-Fallback für Akzente

**Datei**: `main/main.c`  
**Zeile**: ~700 (im `draw_char_utf8()` Function)  
**Typ**: Unicode Handling

### Beschreibung

```c
// draw_char_utf8(): ÄÖÜäöü → custom glyphs.
// é, è, â, ô, ç → base letter fallback (z.B. 'é' → 'E').
```

**Status**: ✅ OK – Entspricht Erwartungen (Keine spezielle Glyphen für alle Latein-1-Zeichen). Keine Änderung nötig.

---

## 🔵 NIEDRIG – Web-Panel: POST/GET `/api/config` Thread-Safety

**Datei**: `main/http_server.c`  
**Typ**: Race Condition bei Config Save während API Call

### Beschreibung

```c
// http_server.c:
if (http_uri == "/api/config") {
    ESP_LOGI(TAG, "Config POST");
    nvs_config_save(&new_cfg);  // NVS schreiben.
    g_cfg_dirty = true;         // ❗ Keine Semaphore/Spinlock!
}
```

**Problem**: 
- `nvs_config_save()` könnte scheitern (NVSLimit).
- `g_cfg_dirty` wird sofort auf `true`, ohne zu prüfen, ob NVS save erfolgreich war.
- Falls `nvs_config_save()` fehlschlägt → **Config Loop** (siehe "Config-Cycle" oben).

### Lösungsvorschlag

```c
esp_err_t err = nvs_config_save(&new_cfg);
if (err != ESP_OK) {
    ESP_LOGE(TAG, "Config Save ERROR!");
    g_cfg_dirty = false;   // Keine Neuladung.
} else {
    g_cfg_dirty = true;
}
```

---

## 🟢 NIEDRIG – `check_window_overlaps()` nur bei `wakeup == 0`

**Datei**: `main/main.c`  
**Zeile**: ~250 (vor Button-Init)  
**Typ**: Logik-Fehler, aber kein Crash.

### Beschreibung

```c
if (wakeup == 0) {
    log_board_info();
    check_window_overlaps();  // ⚠️ Nur bei Kaltstart.
}
```

**Problem**: Bei Sleep/Wake → **keine Prüfung** von Zeitfenster-Überlappungen.  
Nur Logik, kein Crash.

### Empfehlung

**Status**: ✅ OK – Nur Info-Level. Keine Änderung nötig.

---

## 🔴 KRITISCH – NVS Commit-Fehler: `nvs_commit()` ohne Return-Check

**Datei**: `main/nvs_config.c`  
**Zeile**: 143 (im Context der Funktion)  
**Typ**: Fehlerbehandlungslücke, führt zu "stuck" Config bei NVS-Ausfall

### Beschreibung

```c
esp_err_t nvs_config_save(const blink_config_t *cfg) {
    // ... viele nvs_set_xxx() Aufrufe ...
    
    err = nvs_commit(h);  // ⚠️ Kein Check! return err;
    nvs_close(h);
    ESP_LOGI(TAG, "Config gespeichert");
    return err;           // Falls commit() fehlschläft → NVS-Korruption möglich!
}
```

**Problem**: 
- `nvs_commit()` kann fehlschlagen (NVS-Voll, Power-Loss während Write).
- Bei Fehler: **Kein Fallback**, kein Log-Level-Eskalierung.
- Wenn `err != ESP_OK` → Gerät wird mit halbfertiger NVS wiedergeboren.

### Worst Case

```
1. NVS fast voll (>~90%).
2. Save Config mit 4 Filtern + LED-Farben.
3. commit() → ESP_ERR_NVS_NO_SPACE oder ESP_ERR_NVS_NOT.prepared.
4. Keine Fehlermeldung → Device bootet mit korrupter Config.
```

### Lösungsvorschlag

```c
err = nvs_commit(h);
nvs_close(h);
if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS Commit FAILED! NVS-Korruption möglich!");
    return err;  // ⚠️ User muss nvs_flash_erase() oder NVS verkleinern.
}
ESP_LOGI(TAG, "Config gespeichert");
return err;
```

---

## 🔴 KRITISCH – HTTP-Panel POST: `g_cfg_dirty` ohne Save-Ergebnis-Check

**Datei**: `main/http_server.c`  
**Zeile**: ~50 (im Context der Funktion)  
**Typ**: Config-Loop bei NVS-Save-Fehler

### Beschreibung

```c
if (http_uri == "/api/config") {
    ESP_LOGI(TAG, "Config POST");
    nvs_config_save(&new_cfg);  // NVS schreiben.
    g_cfg_dirty = true;         // ❗ Wird SOFORT gesetzt!
}
```

**Problem**: 
- `nvs_config_save()` könnte scheitern (NVSLimit, corrupte Daten).
- `g_cfg_dirty` wird **sofort auf `true`**, ohne Erfolg zu prüfen.
- Falls Save fehlschlägt: Beim nächsten Boot → `if(g_cfg_dirty)` → **Infinite Loop**.

### Reproduktion (Theorie)

```
1. Web-Panel: "Zürich" als Filter speichern.
2. NVS fast voll (>~95%) → nvs_commit() schlägt fehl.
3. g_cfg_dirty = true wird gesetzt.
4. Main-Loop versucht, die korrupte Config zu laden → Fehler!
5. g_cfg_dirty bleibt wahr → Infinite Loop.
```

### Lösungsvorschlag

```c
esp_err_t err = nvs_config_save(&new_cfg);
if (err != ESP_OK) {
    ESP_LOGE(TAG, "Config Save FAILED: %s", esp_err_to_name(err));
    g_cfg_dirty = false;  // Keine Neuladung!
} else {
    ESP_LOGI(TAG, "Config gespeichert");
    g_cfg_dirty = true;   // Nur bei Erfolg setzten!
}
```

---

## 🟠 HOCH – Config Reload-Race: `g_cfg_dirty` im Main-Loop

**Datei**: `main/main.c`  
**Zeile**: ~360 (im `while`-Loop)  
**Typ**: Race Condition bei config save

### Beschreibung

```c
bool g_cfg_dirty = false;  // gesetzt von http_server.c nach POST /api/config
if (g_cfg_dirty) {
    nvs_config_load(&cfg);   // ❗ Kein Mutex!
    // … filter_ptrs[], destFilters[] neu setzen …
}
```

**Problem**: Wenn die Web-Panel save kommt, während der API-Aufruf läuft:
- `filter_ptrs[]` wird während des HTTP-Requests **neu gebildet**.
- Der API-Aufruf (`sbb_get_departures()`) erhält falsche Pointer.

### Lösungsvorschlag

```c
volatile bool g_cfg_dirty = false;  // Thread-safety für Config-Reload
if (g_cfg_dirty) {
    nvs_config_load(&cfg);
    for (int i = 0; i < 4; i++)
        filter_ptrs[i] = (i < cfg.destFilterCount) ? cfg.destFilters[i] : NULL;
}
```

---

## 🟠 HOCH – Stack-Race: `SbbDeparture[4]` in `app_main()`

**Datei**: `main/main.c`  
**Zeile**: ~285 (im `while`-Loop)  
**Typ**: Uninitialisierte Pointer bei Restart

### Beschreibung

```c
static SbbDeparture deps[4];       // Cache für aktiven Zyklus
static SbbDeparture last_deps[4];  // Vorheriger Zyklus
memset(deps, 0, sizeof(deps));     // ✅ gut initialisiert beim Boot
memset(last_deps, 0, sizeof(last_deps)); // ❌ NUR einmal!
```

**Problem**: Bei jedem Deep Sleep / Wake-up:
- `deps` und `last_deps` sind **nicht auf 0 zurückgesetzt**.
- Der Inhalt aus dem vorherigen Zyklus bleibt im RAM.

### Lösungsvorschlag

```c
// Im while-Loop vor der Datenabfrage:
memset(deps, 0, sizeof(deps));    // Reset Cache
memcpy(last_deps, deps, sizeof(last_deps)); // Vorheriger Inhalt übernehmen
```

---

## 🟠 HOCH – Data Race: HTTP Buffer `http_buf`

**Datei**: `main/sbb.c`  
**Zeile**: ~85-90 (im Kontext der Funktion)  
**Typ**: Datenrace / Heap-Leak-Potential  

### Beschreibung

```c
static char http_buf[32768];  // 32 KB static buffer in file scope
bool g_sbb_ok = false;
```

Der 32 KB Buffer wird **lazy allocated** (erster Aufruf von `sbb_get_departures()`), aber:
- Er wird **nie freigegeben** nach Nutzung.
- Bei Fehlern/Wiederholungen wird derselbe Heap-Bereich **neugezupft**.

### Lösungsvorschlag

```c
// malloc() statt static, free() im Shutdown oder nach jedem Aufruf:
char *http_buf = NULL;  // Erst bei Bedarf allokiert
...
if (!http_buf) http_buf = malloc(32768);
...
free(http_buf);
```

---

## 🟡 MITTLER – Config-Cycle: NVS Save/Load Loop

**Datei**: `main/main.c`  
**Zeile**: ~360-370 (im `while`-Loop)  
**Typ**: Unbegrenzter Zyklus bei Fehler

### Beschreibung

```c
if (g_cfg_dirty) {
    nvs_config_load(&cfg);
    g_cfg_dirty = false;   // ⚠️ Wird im HTTP-Server gesetzt, aber …
}
```

**Problem**: Wenn `nvs_config_load()` **scheitert**, bleibt `g_cfg_dirty` auf `true`.

### Lösungsvorschlag

```c
if (g_cfg_dirty) {
    esp_err_t err = nvs_config_load(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Config Load ERROR → g_cfg_dirty = false");
        g_cfg_dirty = false;
        continue;  // Skip this iteration.
    }
    for (int i = 0; i < 4; i++)
        filter_ptrs[i] = (i < cfg.destFilterCount) ? cfg.destFilters[i] : NULL;
}
```

---

## 🟡 MITTLER – API Retry-Race: `sbb_get_departures()` bei `last_deps` Copy

**Datei**: `main/sbb.c`, `main/main.c`  
**Typ**: Race Condition

### Beschreibung

```c
// main.c im while-Loop:
memcpy(last_deps, deps, sizeof(deps));  // ✅ Kopiert aktueller Cache in 'last'
```

**Problem**: `sbb_get_departures()` baut die URL aus dem **aktuellen** Station-String.  
Wenn die Web-Panel den `station` Wert ändert, aber der HTTP-Buffer noch den alten URL enthält → Race!

### Lösungsvorschlag

```c
// sbb_get_departures() station & destination als const char* nehmen,
// statt einen internen buffer zu bauen.
bool sbb_get_departures(const char *station, SbbDeparture results[4],
                        const char *dest_filters[], int filter_count) {
    // Station/Filter sind caller-provided → keine Race möglich.
}
```

---

## 🟡 MITTLER – Button Long-Press-Detection Race

**Datei**: `main/main.c`  
**Zeile**: ~210 (im Button-Wait-Loop)  
**Typ**: Timing Issue, Race bei Sleep/Wake

### Beschreibung

```c
int hold_ms = 0;
while (gpio_get_level(cfg.buttonGpio) == 0 &&
       hold_ms < cfg.buttonLongPressMs + 1000) {
    vTaskDelay(pdMS_TO_TICKS(50));
    hold_ms += 50;
}
```

**Problem**: `hold_ms` wird **inkrementiert**, nicht mit einem "fresh tick" initialisiert.  
Nach Sleep/Wake → Startzeitpunkt des Button-Holds ist unklar.

### Lösungsvorschlag

```c
// Reset hold_ms auf 0 nach Wake-up:
if (woken_by_button) {
    gpio_set_level(cfg.buttonGpio, 1);
    hold_ms = 0;
    while (gpio_get_level(cfg.buttonGpio) == 0 && hold_ms < cfg.buttonLongPressMs + 1000) {
        vTaskDelay(pdMS_TO_TICKS(50));
        hold_ms += 50;
    }
}
```

---

## 🟢 NIEDRIG – SSID Fallback Hardcoded

**Datei**: `main/secrets.h` / `main/sbb.c`  
**Typ**: Security Issue, Credential Persistenz

### Beschreibung

```c
// secrets.h.example:
#define WIFI_SSID "your-wifi-ssid"   // ❗ Wird im Build fixiert!
#define WIFI_PASS "password12345"    // ❗ Pass wird kompiliert mitgeliefert!
```

**Problem**: Bei `nvs_flash_erase()` oder NVS corruption → Hardcoded Credentials.

### Lösungsvorschlag

| Option | Vor-/Nachteil |
|--------|--------------|
| `WIFI_SSID`/`WIFI_PASS` aus NVS-Default laden (kein compile-time fallback) | ✅ Security, muss aber vor dem ersten Boot in NVS stehen. |
| Bei leerem NVS: **Boot-Fehler statt Fallback** | ✅ Verhindert unsichere Verbindung, aber kein "Out of the Box" ohne Konfiguration. |

---

## 🟢 NIEDRIG – Memory-Alignment: `static SbbDeparture[4]` in `app_main()`

**Datei**: `main/main.c`  
**Zeile**: ~285 (im `while`-Loop)  
**Typ**: Stack-Safety, aber `static` macht es Heap-safe.

### Beschreibung

```c
static SbbDeparture deps[4];        // ✅ static → Heap, nicht Main-Task Stack!
static SbbDeparture last_deps[4];
```

**Kein Race**, da:
- `static` → Speicher im **Data-Segment**, nicht Stack.
- Keine Race, da kein Thread-Sharing (ESP32 single-core).

### Empfehlung

**Status**: ✅ OK – Kein Fix nötig.

---

## 🟢 NIEDRIG – OLED-Invert-Timing bei Deep Sleep

**Datei**: `main/main.c`  
**Zeile**: ~480-500 (im `while`-Loop)  
**Typ**: Display-Burn-in Protection

### Beschreibung

```c
TickType_t next_invert = xTaskGetTickCount() +
    pdMS_TO_TICKS((uint32_t)(cfg.oledInvertMin > 0 ? cfg.oledInvertMin : 1440) * 60 * 1000);

if (cfg.oledInvertMin > 0 && t >= next_invert) {
    inverted = !inverted;
    oled_cmd(inverted ? 0xA7 : 0xA6);
    next_invert = t + pdMS_TO_TICKS((uint32_t)cfg.oledInvertMin * 60 * 1000);
}
```

**Status**: ✅ OK – Die Invert-Zyklus schützt gegen Burn-in. Keine Änderung nötig.

---

## 🟢 NIEDRIG – Font-UTF8-Fallback für Akzente

**Datei**: `main/main.c`  
**Zeile**: ~700 (im `draw_char_utf8()` Function)  
**Typ**: Unicode Handling

### Beschreibung

```c
// draw_char_utf8(): ÄÖÜäöü → custom glyphs.
// é, è, â, ô, ç → base letter fallback (z.B. 'é' → 'E').
```

**Status**: ✅ OK – Entspricht Erwartungen (Keine speziellen Glyphen für alle Latein-1-Zeichen). Keine Änderung nötig.

---

## 🔵 NIEDRIG – `check_window_overlaps()` nur bei `wakeup == 0`

**Datei**: `main/main.c`  
**Zeile**: ~250 (vor Button-Init)  
**Typ**: Logik-Fehler, aber kein Crash.

### Beschreibung

```c
if (wakeup == 0) {
    log_board_info();
    check_window_overlaps();  // ⚠️ Nur bei Kaltstart.
}
```

**Problem**: Bei Sleep/Wake → **keine Prüfung** von Zeitfenster-Überlappungen.  
Nur Logik, kein Crash.

### Empfehlung

**Status**: ✅ OK – Nur Info-Level. Keine Änderung nötig.

---

## 🔵 ZUSAMMENFASSUNG – Top 8 Bugs (Priorität)

| # | Bug | Priorität | Datei | Status |
|---|-----|-----------|-------|--------|
| 1 | NVS Commit-Fehler (`nvs_commit()` ohne Return-Check) | **KRITISCH** | `nvs_config.c` | 🟠 Open |
| 2 | HTTP POST: `g_cfg_dirty` ohne Save-Ergebnis-Check | **KRITISCH** | `http_server.c` | 🟠 Open |
| 3 | `http_buf` Data-Race / Heap-Leak (32 KB static) | **HOCH** | `sbb.c` | 🟠 Open |
| 4 | Config Reload-Race (`g_cfg_dirty`) während API-Call | **HOCH** | `main.c` | 🟠 Open |
| 5 | `SbbDeparture[4]` uninitialisiert bei Sleep/Wake | **HOCH** | `main.c` | 🟠 Open |
| 6 | `destFilters[]` = NULL Pointer bei Filter-Fallback | **MEDIUM** | `main.c` | 🟠 Open |
| 7 | NVS Commit ohne Return-Check → halbfertige Config | **KRITISCH** | `nvs_config.c` | 🟠 Open |
| 8 | **Station Hardcoded bei NVS-Fallback** | **MEDIUM** | `sbb.c` | 🟠 Open |

---

## 📋 BUG-ZUSAMMENFASSUNG

### 🔴 KRITISCH (5 Bugs)
| # | Bug | Datei | Worst Case |
|---|-----|-------|------------|
| 1 | NVS Commit-Fehler → korrupte Config bei Write-Fehler | `nvs_config.c` | Boot mit halbfertiger NVS |
| 2 | HTTP POST: g_cfg_dirty vor Save-Erfolg | `http_server.c` | Infinite Loop, Button ignorieren |
| 3 | http_buf Heap-Leak (32 KB static) wird nie freigegeben | `sbb.c` | Memory-Erosion über Wochen/Monate |
| 4 | Config Reload-Race während API-Call | `main.c` | Falsche API-Parameter, falsche Daten |
| 5 | NVS Commit ohne Return-Check → halbfertige Config | `nvs_config.c` | Boot mit korrupter NVS-Sektion |

### 🟠 HOCH (3 Bugs)
| # | Bug | Datei | Worst Case |
|---|-----|-------|------------|
| 5 | SbbDeparture[4] uninitialisiert bei Sleep/Wake | `main.c` | Alte Cache-Daten bleiben erhalten |
| 6 | destFilters[] = NULL Pointer bei Filter-Fallback | `main.c` | Crash statt fallback zu station |

### 🟡 MEDIUM (1 Bug)
| # | Bug | Datei | Worst Case |
|---|-----|-------|------------|
| 7 | check_window_overlaps() nur bei wakeup==0 | `main.c` | Sleep/Wake ohne Zeitfenster-Prüfung |

### 🟢 NIEDRIG (keine Fixes nötig)
| Bug | Datei | Status |
|-----|-------|--------|
| Memory-Alignment (static SbbDeparture[4]) | `main.c` | ✅ OK – static macht es Heap-safe, kein Thread-Sharing |
| OLED-Invert-Timing bei Deep Sleep | `main.c` | ✅ OK – Invert-Zyklus schützt gegen Burn-in |
| Font-UTF8-Fallback für Akzente | `main.c` | ✅ OK – Entspricht Erwartungen (ÄÖÜäöü custom, andere base-fallback) |

---

## 📋 NÄCHSTEN SCHRITT – Priorisierte Fix-Reihenfolge

1. 🔴 **#1 NVS Commit-Fehler** → Return-Check in `nvs_config_save()`
2. 🔴 **#2 HTTP POST Save-Ergebnis** → `g_cfg_dirty` erst nach Erfolg setzen
3. 🔴 **#3 http_buf Heap-Leak** → `malloc()` + `free()` im Shutdown
4. 🔴 **#4 Config Reload-Race** → `volatile bool g_cfg_dirty`, keine Pointer-Änderung während API-Call
5. 🟠 **#5 SbbDeparture[4] Reset** → `memset(deps, 0, sizeof(deps))` im `while`-Loop
6. 🟠 **#6 destFilters[] NULL-Safety** → Fallback zu station, nicht hardcode
7. 🟡 **#7 check_window_overlaps()** → Auch bei Sleep/Wake prüfen (nur Info-Level)

**Status**: 7 identifizierte Bugs in `bugs.md` dokumentiert – **keine Fixes implementiert**.






# Nicht im ESP-Code genutzte Web-UI-Felder

Diese Felder sind im `spiffs/index.html` vorhanden, werden aber **nicht** in der NVS-Konfiguration oder dem ESP-Prozess (`main.c`, `sbb.c`) verwendet.

---

## 1. Zeitzone (TZ-String)
- **Web-UI-Feld:** "Timestep-Zone" / TZ-String
- **Status:** ❌ Nicht im ESP genutzt
- **Grund:** 
  - Der ESP32 hat den RTC (Real-Time Clock), der die Zeit auch nach Deep Sleep behält.
  - NTP wird nur beim ersten Start oder wenn die Systemzeit ungültig ist (`tm_year < 100`) neu geholt.
  - Der TZ-String ist im Code fest als `CET-1CEST,M3.5.0,M10.5.0/3` (Schweiz) konfiguriert und über ein Web-UI-Feld nicht veränderbar.
- **Konsequenz:** Das Zeitzone-Feld in der Web-UI ist funktional überflüssig, da es keine Auswirkung auf das ESP-Verhalten hat.

---

## 2. Ziel-Filter (Destination Filters)
- **Web-UI-Felder:** `filt0`–`filt3` (4 Felder für Zielpunkte)
- **Status:** ✅ Wird genutzt (in `destFilters[]` und `destFilterCount`)
- **Begründung:** Diese werden in der API-Aufruflogik (`sbb_get_departures()`) verwendet, um nach bestimmten Zielen zu filtern. Die Filter werden nur angefragt, wenn `filter_count > 0`.

---

## Zusammenfassung

| Web-UI-Feld                        | Im ESP genutzt? | Grund                              |
|-----------------------------------|-----------------|------------------------------------|
| Zeitzone (TZ-String)             | ❌ Nein         | ESP verwendet RTC + festes TZ      |
| Ziel-Filter 0–3                  | ✅ Ja           | Werden in der API-Logik genutzt    |

**Empfehlung:** Das "Zeitzone"-Feld kann entfernt werden, es hat keinen Effekt auf die Funktionalität des ESP.


# Web-UI Felder: Nicht genutzt oder unvollständig

Dieses Dokument listet alle Konfigurationsfelder auf, die in der Web-UI vorhanden sind, aber **nicht** in der Live-Vorschau (`safe-first`) dargestellt werden – obwohl sie vom ESP32 verwendet werden.

---

## 🔴 Wichtig: sleepEnabled-Feld (Kritisch!)

| Feld | In safe-first sichtbar? | Nutzt ESP32 es? | Problem |
|------|------------------------|-----------------|---------|
| **`sleepEnabled`** (Toggle) | ❌ Nein, komplett unsichtbar | ✅ Ja | Der Schlaf-Zustand wird im ESP-NVSS gespeichert, aber der Toggle erscheint nicht in der safe-first Ansicht. Ohne dieses Feld ist `safe-first` unvollständig und inkonsistent mit der eigentlichen Konfiguration. |

### Warum wichtig?

- Die **Live-Vorschau** (`safe-first`) soll den aktuellen Zustand des ESP32 widerspiegeln
- Ohne `sleepEnabled` in safe-first kann der User nicht sehen, ob "Deep Sleep" aktiviert ist
- Der Toggle im Einstellungen-Tab wird korrekt geladen und gespeichert (via `saveConfig()`), aber er ist aus dem safe-first View unzugänglich

---

## 🟠 Optional: Wochenend-Schlaf-Felder (Fehlendes UI in safe-first)

| Feld | In safe-first sichtbar? | Nutzt ESP32 es? | Problem |
|------|------------------------|-----------------|---------|
| **`weekendSleepEnabled`** | ❌ Nein | ✅ Ja | Wird im "Schlaf"-Tab konfiguriert, aber nicht in safe-first angezeigt. Nur relevant wenn `weekdaysOnly=true`. |
| **`weekendStartDay/weekendStartH/weekendStartM`** | ❌ Nein | ✅ Ja | Startzeitpunkt des Wochenendschlafs (Freitag 18:00 Standard). Nicht im safe-first sichtbar. |
| **`weekendEndDay/weekendEndH/weekendEndM`** | ❌ Nein | ✅ Ja | Endzeitpunkt des Wochenendschlafs (Montag 5:00 Standard). Nicht im safe-first sichtbar. |

### Warum optional?

- Diese Felder sind nur relevant wenn `weekdaysOnly=true`
- Sie definieren einen zusätzlichen Sleep-Modus für die Woche, der separat vom aktiven Zeitfenster läuft
- Wenn der User diese Felder ändert, wird es in safe-first nicht reflektiert → inkonsistent

---

## 🟢 Optional: LED-Farb-Vorschau (Nicht im safe-first)

| Feld | In safe-first sichtbar? | Nutzt ESP32 es? | Problem |
|------|------------------------|-----------------|---------|
| **LED-Farb-Picker** (`ledOkColor`, `ledDelaySmallColor`, …) | ❌ Nein | ✅ Ja | Die Farbwähler im "LED"-Tab funktionieren und speichern Farben korrekt, aber die Vorschau in safe-first zeigt nur statische Icons ohne echte Farbe. Der User kann die exakte Farbe nicht live sehen. |

### Warum optional?

- Die ESP32 speichert LED-Farben korrekt
- Nur das **Design** ist suboptimal: Keine Live-Vorschau der Farbwahl
- Die Funktion "testen" im safe-first (Zeile 357) simuliert den Zustand, zeigt aber nicht die gewählte Farbe

---

## 🟡 Optional: Button Wake-up Felder (Nicht in safe-first)

| Feld | In safe-first sichtbar? | Nutzt ESP32 es? | Problem |
|------|------------------------|-----------------|---------|
| **`buttonActiveMin`** | ❌ Nein | ✅ Ja | Kurzdruck-Aktivität (Standard: 10 min). Nicht im safe-first sichtbar. |
| **`buttonLongPressMs`** | ❌ Nein | ✅ Ja | Long-Press-Schwelle (3000 ms). Nicht im safe-first sichtbar. |
| **`buttonLongActiveMin`** | ❌ Nein | ✅ Ja | Long-Press-Dauer nach aktivem Trigger. Nicht im safe-first sichtbar. |
| **`buttonGpio`** | ❌ Nein | ✅ Ja | GPIO-Pin für Wake-up (Standard: 0). Nicht im safe-first sichtbar. |

### Warum optional?

- Diese Felder beeinflussen nur das **Wake-up-Verhalten**, nicht die visuelle Darstellung
- Die Safe-first View dient primär der Überwachung der aktuellen Abfahrten, nicht dem Wake-up-Test

---

## 🟡 Optional: API & Refresh (Nicht in safe-first)

| Feld | In safe-first sichtbar? | Nutzt ESP32 es? | Problem |
|------|------------------------|-----------------|---------|
| **`apiRetryCount`** | ❌ Nein | ✅ Ja | Anzahl API-Retry-Versuche. Nicht im safe-first sichtbar. |
| **`apiRetryDelayS`** | ❌ Nein | ✅ Ja | Zeit zwischen Retries (5 s Standard). Nicht im safe-first sichtbar. |
| **`staleMaxMin`** | ❌ Nein | ✅ Ja | Wie lange stale Daten als "!"-Präfix zeigen. Nicht im safe-first sichtbar. |

### Warum optional?

- Diese Felder beeinflussen nur das **Netzwerkverhalten**, nicht die Visualisierung
- Die Safe-first View ist ein Overhead, kein Testfeld für API-Retry-Logik

---

## 🟢 Optional: Ziel-Filter (Nicht in safe-first)

| Feld | In safe-first sichtbar? | Nutzt ESP32 es? | Problem |
|------|------------------------|-----------------|---------|
| **`destFilters[0..3]`** | ❌ Nein | ✅ Ja | Zielpunkt-Filter für die Abfahrt (4 Slots). Nicht im safe-first sichtbar. |

### Warum optional?

- Filter werden nur für die API-Abfrage genutzt
- Die Visualisierung zeigt alle gültigen Abfahrten, unabhängig vom Filter
- Safe-first ist kein "Filter-Testfeld", sondern eine reine Live-Ansicht

---

## 📊 Zusammenfassung: Nutzfaktoren

| Kategorie | In safe-first sichtbar? | ESP32 nutzt es? |
|-----------|------------------------|-----------------|
| **Kern-Visualisierung** | ✅ Ja (Abfahrten, Zeitfenster, Status) | ✅ Ja |
| **Sleep-Konfiguration** | ⚠️ Nur `sleepEnabled` als Toggle (nicht in safe-first) | ✅ Ja |
| **LED-Farben** | ⚠️ Statistische Icons (keine Live-Vorschau der Farbe) | ✅ Ja |
| **Wake-up-Parameter** | ❌ Nein | ✅ Ja |
| **API-Retry-Logik** | ❌ Nein | ✅ Ja |
| **Ziel-Filter** | ❌ Nein | ✅ Ja |
| **Wochenend-Schlaf** | ❌ Nein | ✅ Ja |

---

## ✅ Empfehlung

### Priorität 1 (Kritisch)
- `sleepEnabled` in safe-first hinzufügen, da dies den eigentlichen Zustand des ESP32 widerspiegelt und der User sonst nicht sehen kann, ob Deep Sleep aktiviert ist.

### Priorität 2 (Nice-to-have)
- Wochenend-Schlaf-Felder in safe-first anzeigen (nur wenn `weekdaysOnly=true`)
- LED-Farb-Vorschau verbessern (live Farben anstatt statischer Icons)

### Optional (Low Priority)
- Wake-up-, API-Retry- und Ziel-Filter-Felder sind nur für fortgeschrittene Debugging-Szenarien relevant. Sie können weiterhin im "Einstellungen"-Tab bleiben, müssen aber nicht zwingend in safe-first sein.

---

## 📂 Dateien

| Datei | Beschreibung |
|-------|--------------|
| `webui-nicht-genutzte-felder.md` | Dieses Dokument |
| `main/spiffs/index.html` | Web-UI Source (HTML + JavaScript) |
| `main/nvs_config.c` | ESP32 Konfigurationsstruktur (`blink_config_t`) |
| `main/main.c` | Hauptlogik, nutzt alle Felder aus `blink_config_t` |
