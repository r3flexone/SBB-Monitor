# Web-Panel Integration — Spec für Claude Design

## Kontext

ESP32-S3 SBB-Abfahrtsmonitor. Code in `main/main.c`, `main/sbb.c/.h`. Web-Panel-Entwurf liegt in `esp32/` (index.html, http_server, nvs_config). Panel soll auf dem ESP32 laufen unter `http://blink.local` (nur wenn Gerät wach ist).

**Strom:** Vernachlässigbar — WiFi läuft bereits, HTTP-Server idle ~5 mA extra.

---

## Fehlende Config-Felder

Das Web-Panel hat 17 Felder in `blink_config_t`. `main.c` hat aber ~43 konfigurierbare Werte. **Folgende fehlen:**

| Feld | Typ | Default | Beschreibung |
|------|-----|---------|-------------|
| `timeWindows[8]` | `struct{u8 sh,sm,eh,em}` | `{6,45,6,55}` | Multiple Zeitfenster (aktuell nur 1) |
| `timeWindowCount` | `int` | 1 | Anzahl aktiver Fenster |
| `destFilters[4][32]` | `char[][]` | leer | Ziel-Filter (Substring, case-insensitive) |
| `destFilterCount` | `int` | 0 | Anzahl Filter (0=alle Züge) |
| `weekdaysOnly` | `bool` | true | Nur Mo–Fr aktiv |
| `buttonLongPressMs` | `int` | 3000 | Long-press Schwelle |
| `buttonLongActiveMin` | `int` | 10 | Dauer nach Long-press |
| `oledInvertMin` | `int` | 5 | Burn-in Schutz Intervall (0=aus) |
| `apiRetryCount` | `int` | 3 | API-Versuche pro Refresh |
| `apiRetryDelayMs` | `int` | 5000 | Pause zwischen Retries |
| `staleMaxMin` | `int` | 10 | Cache-Gültigkeit (Min) |
| `refreshNearSec` | `int` | 30 | Refresh wenn Zug <5 Min |
| `refreshMidSec` | `int` | 120 | Refresh wenn Zug 5–10 Min |
| `refreshFarSec` | `int` | 300 | Refresh wenn Zug 10–30 Min |
| `refreshVeryfarSec` | `int` | 600 | Refresh wenn Zug >30 Min |
| `refreshNearMin` | `int` | 5 | Schwelle near/mid |
| `refreshMidMin` | `int` | 10 | Schwelle mid/far |
| `refreshFarMin` | `int` | 30 | Schwelle far/veryfar |
| `delaySmallMin` | `int` | 2 | Ab wann "leichte Verspätung" |
| `delayBigMin` | `int` | 6 | Ab wann "grosse Verspätung" |
| `ledErrorBlinkMs` | `int` | 500 | Error-Blink Periode (0=dauerhaft) |
| `ledOkR/G/B` | `uint8_t` je 3 | 0,255,0 | LED pünktlich (grün) |
| `ledDelaySmallR/G/B` | `uint8_t` je 3 | 0,255,255 | LED leicht verspätet (cyan) |
| `ledDelayBigR/G/B` | `uint8_t` je 3 | 128,0,255 | LED stark verspätet (lila) |
| `ledCancelledR/G/B` | `uint8_t` je 3 | 255,0,0 | LED Ausfall (rot) |
| `ledLoadingR/G/B` | `uint8_t` je 3 | 255,128,0 | LED laden (orange) |

---

## Zu ändernde Dateien

| Datei | Aktion |
|-------|--------|
| `main/nvs_config.h` | `blink_config_t` um obige Felder erweitern |
| `main/nvs_config.c` | LOAD/SAVE für alle neuen Felder, Defaults setzen |
| `main/http_server.h/.c` | Aus `esp32/main/` kopieren, Handler für alle Felder erweitern |
| `main/spiffs/index.html` | Aus `esp32/web/` kopieren, neue Sections hinzufügen |
| `main/CMakeLists.txt` | SRCS: `http_server.c nvs_config.c`, REQUIRES: `esp_http_server spiffs mdns` hinzu, `spiffs_create_partition_image()` |
| `partitions.csv` | Neu: nvs 24K, phy 4K, factory 1500K, storage(spiffs) 256K |
| `sdkconfig.defaults` | `CONFIG_PARTITION_TABLE_CUSTOM=y` + Filename |
| `main/main.c` | NVS init ganz oben, nach WiFi: `mdns_init()` + `http_server_start()`, vor Sleep: `http_server_stop()` |

---

## HTML-Panel: Neue Sections

Bestehend: Zeitfenster (1x), Netzwerk, Schlaf, Hardware, Status

Neu hinzufügen:
1. **Zeitfenster** → Multiple (max 8), mit +/- Buttons, Overlap-Warnung
2. **Ziel-Filter** → 4× Text-Input, "leer = alle Züge"
3. **LED-Farben** → RGB-Inputs × 5 Zustände + Delay-Schwellen
4. **API & Refresh** → Retry-Count/Delay, Stale-Max, 4 Refresh-Tiers
5. **Verhalten** → weekdaysOnly, oledInvert, buttonLongPress, errorBlink

---

## Wichtige Hinweise

- **NVS-Konflikt:** `sbb_wifi_init()` ruft auch `nvs_flash_init()` auf — doppelter Aufruf ist OK (returns `ESP_ERR_NVS_NO_FREE_PAGES` nicht), aber Reihenfolge beachten
- **Passwort:** GET `/api/config` gibt Passwort NICHT zurück
- **buttonActiveS:** Web-Panel nutzt Sekunden, `main.c` nutzt Minuten (`BUTTON_ACTIVE_MIN`) — vereinheitlichen
- **INTEGRATION.md:** Referenziert `blink_example_main.c` — heisst jetzt `main.c`
- **Stack:** Main-Task hat nur ~3584 Bytes — `blink_config_t` als `static` oder auf dem Heap
- **`idf.py fullclean`** nötig nach Partition-Table-Änderung
